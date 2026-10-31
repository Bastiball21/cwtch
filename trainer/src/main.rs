use bullet_lib::{
    game::{
        inputs::{ChessBucketsMirrored, get_num_buckets},
        outputs::MaterialCount,
    },
    nn::{
        InitSettings, Shape,
        optimiser::{AdamW, AdamWParams},
    },
    trainer::{
        save::SavedFormat,
        schedule::{TrainingSchedule, TrainingSteps, lr, wdl},
        settings::LocalSettings,
    },
    value::ValueTrainerBuilder,
};

use bullet_lib::game::formats::bulletformat::ChessBoard;
use bullet_lib::value::loader::sfbinpack::{TrainingDataEntry, Color, PieceType};
use bullet_trainer::reader::DataReader;
use sfbinpack::{ChunkReader, read_chunk_into};
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;
use std::sync::atomic::{AtomicI32, Ordering};
use rand::seq::SliceRandom;
use rand::thread_rng;

// ---------------------------------------------------------------------------
// Global score filter for the binpack filter function
// ---------------------------------------------------------------------------
static SF_SCORE_FILTER: AtomicI32 = AtomicI32::new(1500);

// ---------------------------------------------------------------------------
// .vf.zst decoder (packed board + move replay)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct PackedBoard {
    pieces: [u8; 64],
    stm: u8,
    ep: u8,
}

const EMPTY: u8 = 12;

impl PackedBoard {
    fn from_bytes(data: &[u8; 32]) -> Self {
        let occ = u64::from_le_bytes(data[0..8].try_into().unwrap());
        let mut pieces = [EMPTY; 64];
        let nibbles = &data[8..24];
        let mut idx = 0;
        let mut occ_copy = occ;
        while occ_copy != 0 {
            let sq = occ_copy.trailing_zeros() as usize;
            occ_copy &= occ_copy - 1;
            let byte = nibbles[idx / 2];
            let nibble = if idx % 2 == 0 { byte & 0x0F } else { byte >> 4 };
            let color = (nibble >> 3) & 1;
            let piece_type = nibble & 7;
            let piece = piece_type + color * 6;
            let piece = if piece_type == 6 { 3 + color * 6 } else { piece };
            pieces[sq] = piece as u8;
            idx += 1;
        }
        let stm = (data[24] >> 7) & 1;
        let ep = data[24] & 0x7F;
        PackedBoard { pieces, stm, ep }
    }

    fn to_chessboard(&self, score: i16, result: f32) -> Option<ChessBoard> {
        let mut bbs = [0u64; 8];
        let mut white = 0u64;
        let mut black = 0u64;

        for sq in 0..64 {
            let p = self.pieces[sq];
            if p == EMPTY { continue; }
            let bit = 1u64 << sq;
            let color = p / 6;
            let ptype = p % 6;
            if color == 0 { white |= bit; } else { black |= bit; }
            let mapped = match ptype {
                0 => 2, 1 => 3, 2 => 4, 3 => 5, 4 => 6, 5 => 7, _ => 2,
            };
            bbs[mapped] |= bit;
        }
        bbs[0] = white;
        bbs[1] = black;

        let white_kings = bbs[0] & bbs[7];
        let black_kings = bbs[1] & bbs[7];
        if white_kings.count_ones() != 1 || black_kings.count_ones() != 1 {
            return None;
        }

        let stm = self.stm as usize;
        ChessBoard::from_raw(bbs, stm, score, result).ok()
    }

    fn apply_move(&mut self, mv: u16) {
        let from = (mv & 0x3F) as usize;
        let to = ((mv >> 6) & 0x3F) as usize;
        let mtype = (mv >> 14) & 0x3;
        let promo = (mv >> 12) & 0x3;

        match mtype {
            0 => {
                self.pieces[to] = self.pieces[from];
                self.pieces[from] = EMPTY;
            }
            1 => {
                self.pieces[to] = self.pieces[from];
                self.pieces[from] = EMPTY;
                let cap_sq = if self.stm == 0 { to - 8 } else { to + 8 };
                self.pieces[cap_sq] = EMPTY;
            }
            2 => {
                let king_from = from;
                let rook_from = to;
                let (king_to, rook_to) = if to > from {
                    if self.stm == 0 { (6, 5) } else { (62, 61) }
                } else {
                    if self.stm == 0 { (2, 3) } else { (58, 59) }
                };
                let kp = self.pieces[king_from];
                let rp = self.pieces[rook_from];
                self.pieces[king_from] = EMPTY;
                self.pieces[rook_from] = EMPTY;
                self.pieces[king_to] = kp;
                self.pieces[rook_to] = rp;
            }
            3 => {
                let promo_piece = match promo {
                    0 => 1, 1 => 2, 2 => 3, 3 => 4, _ => 4,
                };
                let color = self.stm;
                self.pieces[to] = promo_piece + color * 6;
                self.pieces[from] = EMPTY;
            }
            _ => {}
        }
        self.stm ^= 1;
        self.ep = 64;
    }
}

// ---------------------------------------------------------------------------
// Convert an sfbinpack entry to a bulletformat ChessBoard
// ---------------------------------------------------------------------------

fn convert_sf_entry(entry: &TrainingDataEntry) -> ChessBoard {
    let mut bbs = [0u64; 8];
    let stm = usize::from(entry.pos.side_to_move().ordinal());
    let pc_bb = |pt| {
        entry.pos.pieces_bb_color(Color::Black, pt).bits()
            | entry.pos.pieces_bb_color(Color::White, pt).bits()
    };

    bbs[0] = entry.pos.pieces_bb(Color::White).bits();
    bbs[1] = entry.pos.pieces_bb(Color::Black).bits();
    bbs[2] = pc_bb(PieceType::Pawn);
    bbs[3] = pc_bb(PieceType::Knight);
    bbs[4] = pc_bb(PieceType::Bishop);
    bbs[5] = pc_bb(PieceType::Rook);
    bbs[6] = pc_bb(PieceType::Queen);
    bbs[7] = pc_bb(PieceType::King);

    let mut score = entry.score;
    let mut result = f32::from(1 + entry.result) / 2.0;
    if stm > 0 {
        score = -score;
        result = 1.0 - result;
    }

    ChessBoard::from_raw(bbs, stm, score, result).expect("sfbinpack entry malformed")
}

fn sf_default_filter(entry: &TrainingDataEntry) -> bool {
    let max_score = SF_SCORE_FILTER.load(Ordering::Relaxed);
    if entry.ply < 16 { return false; }
    if entry.pos.is_checked(entry.pos.side_to_move()) { return false; }
    if (entry.score as i32).abs() > max_score { return false; }
    true
}

// ---------------------------------------------------------------------------
// Mixed loader: reads .vf.zst and .binpack in one interleaved stream
// ---------------------------------------------------------------------------

#[derive(Clone)]
pub struct MixedLoader {
    vf_files: Vec<String>,
    sf_files: Vec<String>,
    buffer_size: usize,
    score_filter: i32,
}

impl MixedLoader {
    pub fn new(vf_files: Vec<String>, sf_files: Vec<String>, buffer_mb: usize, score_filter: i32) -> Self {
        let boards = (buffer_mb * 1024 * 1024) / std::mem::size_of::<ChessBoard>();
        Self { vf_files, sf_files, buffer_size: boards.max(1024), score_filter }
    }

    fn read_vf_file(
        &self,
        path: &str,
        chunk: &mut Vec<ChessBoard>,
    ) -> Result<(), String> {
        let file = File::open(path).map_err(|e| e.to_string())?;
        let mut reader: Box<dyn Read> = if path.ends_with(".zst") {
            Box::new(zstd::stream::read::Decoder::new(file).map_err(|e| e.to_string())?)
        } else {
            Box::new(file)
        };

        loop {
            let mut header = [0u8; 35];
            if reader.read_exact(&mut header).is_err() { break; }
            let mut board = PackedBoard::from_bytes(&header[0..32].try_into().unwrap());
            let wdl = header[32];
            let count = u16::from_le_bytes([header[33], header[34]]);
            if count > 1024 {
                return Err(format!("record count {} too large", count));
            }

            let result = match wdl { 0 => 0.0, 1 => 0.5, 2 => 1.0, _ => 0.5 };

            for _ in 0..count {
                let mut rec = [0u8; 4];
                if reader.read_exact(&mut rec).is_err() {
                    return Err("truncated mid-game".to_string());
                }
                let mv = u16::from_le_bytes([rec[0], rec[1]]);
                let score = i16::from_le_bytes([rec[2], rec[3]]);

                // Check score filter BEFORE building the ChessBoard bitboards
                if score != i16::MIN && (score as i32).abs() <= self.score_filter {
                    if let Some(cb) = board.to_chessboard(score, result) {
                        chunk.push(cb);
                    }
                }
                board.apply_move(mv);
            }
        }
        Ok(())
    }

    fn read_sf_chunk(
        reader: &mut BufReader<File>,
        buf: &mut Vec<u8>,
        chunk: &mut Vec<ChessBoard>,
    ) -> Result<bool, String> {
        buf.clear();
        match read_chunk_into(reader, buf) {
            Ok(true) => {}
            Ok(false) => return Ok(false),
            Err(e) => return Err(format!("{:?}", e)),
        }
        let mut cr = ChunkReader::default();
        while cr.has_next(buf) {
            let entry = cr.next(buf);
            if sf_default_filter(&entry) {
                chunk.push(convert_sf_entry(&entry));
            }
        }
        Ok(true)
    }
}

impl DataReader<ChessBoard> for MixedLoader {
    fn read_chunks<F: FnMut(&[ChessBoard]) -> bool>(&self, _: usize, mut f: F) {
        if self.vf_files.is_empty() && self.sf_files.is_empty() {
            panic!("MixedLoader: no .vf.zst or .binpack files found");
        }

        let mut vf_order: Vec<usize> = (0..self.vf_files.len()).collect();
        vf_order.shuffle(&mut thread_rng());
        let mut vf_cursor = 0usize;

        let mut sf_readers: Vec<(usize, Option<BufReader<File>>)> = self
            .sf_files
            .iter()
            .enumerate()
            .map(|(i, p)| (i, File::open(p).ok().map(BufReader::new)))
            .collect();
        sf_readers.shuffle(&mut thread_rng());

        let mut buf: Vec<u8> = Vec::new();
        let mut chunk: Vec<ChessBoard> = Vec::with_capacity(self.buffer_size + 65536);
        let mut pass: u64 = 0;

        loop {
            let mut progressed = false;

            // Round-robin 1 binpack chunk from each active .binpack stream
            // and 1 .vf.zst file per step so every shuffle buffer mixes all sources.
            for (sf_idx, reader_opt) in sf_readers.iter_mut() {
                if reader_opt.is_none() {
                    *reader_opt = File::open(&self.sf_files[*sf_idx]).ok().map(BufReader::new);
                }
                if let Some(reader) = reader_opt {
                    match Self::read_sf_chunk(reader, &mut buf, &mut chunk) {
                        Ok(true) => progressed = true,
                        Ok(false) => {
                            *reader_opt = None;
                        }
                        Err(e) => {
                            eprintln!("warn: {}: {}", self.sf_files[*sf_idx], e);
                            *reader_opt = None;
                        }
                    }
                }

                if !vf_order.is_empty() {
                    let vf_idx = vf_order[vf_cursor];
                    vf_cursor += 1;
                    if vf_cursor >= vf_order.len() {
                        vf_order.shuffle(&mut thread_rng());
                        vf_cursor = 0;
                    }
                    let vf_path = &self.vf_files[vf_idx];
                    match self.read_vf_file(vf_path, &mut chunk) {
                        Ok(()) => progressed = true,
                        Err(e) => eprintln!("warn: {}: {}", vf_path, e),
                    }
                }

                if chunk.len() >= self.buffer_size {
                    chunk.shuffle(&mut thread_rng());
                    if f(&chunk) { return; }
                    chunk.clear();
                }
            }

            // Handle case where only .vf.zst files exist (no .binpack files)
            if self.sf_files.is_empty() && !vf_order.is_empty() {
                let vf_idx = vf_order[vf_cursor];
                vf_cursor += 1;
                if vf_cursor >= vf_order.len() {
                    vf_order.shuffle(&mut thread_rng());
                    vf_cursor = 0;
                }
                let vf_path = &self.vf_files[vf_idx];
                match self.read_vf_file(vf_path, &mut chunk) {
                    Ok(()) => progressed = true,
                    Err(e) => eprintln!("warn: {}: {}", vf_path, e),
                }
                if chunk.len() >= self.buffer_size {
                    chunk.shuffle(&mut thread_rng());
                    if f(&chunk) { return; }
                    chunk.clear();
                }
            }

            if !progressed {
                pass += 1;
                eprintln!("[MixedLoader] completed pass {}", pass);
                sf_readers.shuffle(&mut thread_rng());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Directory scan
// ---------------------------------------------------------------------------

fn scan_data_dir(path: &str) -> (Vec<String>, Vec<String>) {
    let mut vf = Vec::new();
    let mut sf = Vec::new();
    let p = Path::new(path);

    if p.is_dir() {
        for entry in std::fs::read_dir(path).unwrap() {
            let entry = entry.unwrap();
            let ep = entry.path();
            let name = ep.file_name().and_then(|s| s.to_str()).unwrap_or("");
            if name.ends_with(".binpack") {
                // Skip .part_N.binpack if the unsplit .binpack file also exists
                if let Some(pos) = name.find(".part_") {
                    let base_name = format!("{}.binpack", &name[..pos]);
                    if p.join(&base_name).exists() {
                        println!("Skipping duplicate split {} (unsplit {} exists)", name, base_name);
                        continue;
                    }
                }
                sf.push(ep.to_string_lossy().to_string());
            } else if name.ends_with(".vf.zst") || name.ends_with(".vf") {
                vf.push(ep.to_string_lossy().to_string());
            }
        }
    } else {
        let name = p.file_name().and_then(|s| s.to_str()).unwrap_or("");
        if name.ends_with(".binpack") {
            sf.push(path.to_string());
        } else {
            vf.push(path.to_string());
        }
    }
    vf.sort();
    sf.sort();
    (vf, sf)
}

// ---------------------------------------------------------------------------
// Trainer configuration
// ---------------------------------------------------------------------------

fn get_env_usize(name: &str, default: usize) -> usize {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn get_env_f32(name: &str, default: f32) -> f32 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn get_env_i32(name: &str, default: i32) -> i32 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

const OUTPUT_DIR: &str = "nets";
const SCALE: i32 = 400;
const QA: i16 = 255;
const QB: i16 = 64;
const BUFFER_MB: usize = 256;
const NUM_OUTPUT_BUCKETS: usize = 8;

#[rustfmt::skip]
const BUCKET_LAYOUT: [usize; 32] = [
    0,  0,  1,  1,
    2,  2,  3,  3,
    4,  4,  5,  5,
    6,  6,  7,  7,
    8,  8,  9,  9,
    10, 10, 11, 11,
    12, 12, 13, 13,
    14, 14, 15, 15,
];

fn normalize_data_path(raw: String) -> String {
    #[cfg(target_os = "linux")]
    {
        let bytes = raw.as_bytes();
        if bytes.len() >= 3 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':' && (bytes[2] == b'\\' || bytes[2] == b'/') {
            let drive = (bytes[0] as char).to_ascii_lowercase();
            let rest = raw[3..].replace('\\', "/");
            return format!("/mnt/{}/{}", drive, rest);
        }
    }
    raw
}

fn main() {
    let data_path              = normalize_data_path(std::env::var("DATA_PATH").unwrap_or_else(|_| "/mnt/d/Bulk-Data".to_string()));
    let hidden_size            = get_env_usize("HIDDEN_SIZE", 1536);
    assert!(hidden_size > 0 && hidden_size % 32 == 0 && hidden_size <= 2048,
            "HIDDEN_SIZE must be a multiple of 32 and <= 2048");
    let batch_size             = get_env_usize("BATCH_SIZE", 16384);
    let start_superbatch       = get_env_usize("START_SUPERBATCH", 1);
    let end_superbatch         = get_env_usize("SUPERBATCH", 600);
    let batches_per_superbatch = get_env_usize("BATCHES_PER_SUPERBATCH", 6104);
    let save_rate              = get_env_usize("SAVE_RATE", 50);
    let train_threads          = get_env_usize("TRAIN_THREADS", 24);
    let initial_lr             = get_env_f32("LEARNING_RATE", 0.001);
    let warmup_batches         = get_env_usize("WARMUP_BATCHES", 200);
    let score_filter           = get_env_i32("SCORE_FILTER", 1500);
    let wdl_value              = get_env_f32("WDL_VALUE", 0.4);
    let checkpoint_load        = std::env::var("LOAD_CHECKPOINT").ok();
    let checkpoint_save        = std::env::var("SAVE_CHECKPOINT").ok();

    SF_SCORE_FILTER.store(score_filter, Ordering::Relaxed);
    let num_input_buckets = get_num_buckets(&BUCKET_LAYOUT);

    let (vf_files, sf_files) = scan_data_dir(&data_path);
    println!(
        "Detected {} .vf.zst files and {} .binpack files",
        vf_files.len(), sf_files.len()
    );
    if vf_files.is_empty() && sf_files.is_empty() {
        panic!("DATA_PATH {} contains no .vf.zst or .binpack files", data_path);
    }
    let loader = MixedLoader::new(vf_files, sf_files, BUFFER_MB, score_filter);

    println!("--- Trainer configuration ---");
    println!("DATA_PATH              : {}", data_path);
    println!("HIDDEN_SIZE            : {}", hidden_size);
    println!("BATCH_SIZE             : {}", batch_size);
    println!("START_SUPERBATCH       : {}", start_superbatch);
    println!("END_SUPERBATCH         : {}", end_superbatch);
    println!("BATCHES_PER_SUPERBATCH : {}", batches_per_superbatch);
    println!("SAVE_RATE              : {}", save_rate);
    println!("TRAIN_THREADS          : {}", train_threads);
    println!("LEARNING_RATE          : {}", initial_lr);
    println!("WARMUP_BATCHES         : {}", warmup_batches);
    println!("SCORE_FILTER           : {}", score_filter);
    println!("WDL_VALUE              : {}", wdl_value);
    println!("LOAD_CHECKPOINT        : {}", checkpoint_load.as_deref().unwrap_or("(none)"));
    println!("SAVE_CHECKPOINT        : {}", checkpoint_save.as_deref().unwrap_or("(none)"));
    println!("-----------------------------");

    let mut trainer = ValueTrainerBuilder::default()
        .dual_perspective()
        .optimiser(AdamW)
        .inputs(ChessBucketsMirrored::new(BUCKET_LAYOUT))
        .output_buckets(MaterialCount::<NUM_OUTPUT_BUCKETS>)
        .save_format(&[
            // Merge shared factoriser l0f into each king bucket's l0w weights on save
            SavedFormat::id("l0w")
                .transform(move |store, weights| {
                    let factoriser = store.get("l0f").values.f32().repeat(num_input_buckets);
                    weights.into_iter().zip(factoriser).map(|(a, b)| a + b).collect()
                })
                .round()
                .quantise::<i16>(QA),
            SavedFormat::id("l0b").round().quantise::<i16>(QA),
            SavedFormat::id("l1w")
                .transform(|_, weights| {
                    // Enforce |w| <= 127 / 64 = 1.984375 so quantized |w| <= 127
                    weights.into_iter().map(|w| w.clamp(-127.0 / (QB as f32), 127.0 / (QB as f32))).collect()
                })
                .round()
                .quantise::<i16>(QB)
                .transpose(),
            SavedFormat::id("l1b").round().quantise::<i16>(QA * QB),
        ])
        .loss_fn(|output, target| output.sigmoid().squared_error(target))
        .build(|builder, stm_inputs, ntm_inputs, output_buckets| {
            // Zero-init factoriser shared across all king buckets, merged into l0w on save
            let l0f = builder.new_weights("l0f", Shape::new(hidden_size, 768), InitSettings::Zeroed);
            let expanded_factoriser = l0f.repeat(num_input_buckets);

            let mut l0 = builder.new_affine("l0", 768 * num_input_buckets, hidden_size);
            l0.weights = l0.weights + expanded_factoriser;

            let l1 = builder.new_affine("l1", 2 * hidden_size, NUM_OUTPUT_BUCKETS);

            let stm_hidden = l0.forward(stm_inputs).screlu();
            let ntm_hidden = l0.forward(ntm_inputs).screlu();
            let hidden_layer = stm_hidden.concat(ntm_hidden);
            l1.forward(hidden_layer).select(output_buckets)
        });

    // Clip l0w and l0f to [-0.99, 0.99] (sum in [-1.98, 1.98]) and l1w to [-1.98, 1.98] (|w_q| <= 127)
    let l0_clipping = AdamWParams { max_weight: 0.99, min_weight: -0.99, ..Default::default() };
    let l1_clipping = AdamWParams { max_weight: 1.98, min_weight: -1.98, ..Default::default() };
    trainer.optimiser.set_params_for_weight("l0w", l0_clipping);
    trainer.optimiser.set_params_for_weight("l0f", l0_clipping);
    trainer.optimiser.set_params_for_weight("l1w", l1_clipping);

    if let Some(ref path) = checkpoint_load {
        trainer.load_from_checkpoint(path);
        println!("Loaded checkpoint from {}", path);
    }

    if let Ok(init_dir) = std::env::var("SAVE_INIT_NET") {
        std::fs::create_dir_all(&init_dir).expect("failed to create SAVE_INIT_NET dir");
        trainer.save_to_checkpoint(&init_dir);
        println!("Saved initial checkpoint to {}", init_dir);
        for fen in [
            "r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 1",
            "4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 0 1",
            "r3qbrk/6p1/2b2pPp/p3pP1Q/PpPpP2P/3P1B2/2PB3K/R5R1 w - - 0 1",
            "8/8/1p2k1p1/3p3p/1p1P1P1P/1P2PK2/8/8 w - - 0 1",
        ] {
            println!("eval {} cp: {fen}", (SCALE as f32 * trainer.eval(fen)).round());
        }
        return;
    }

    let schedule = TrainingSchedule {
        net_id: "cwtch".to_string(),
        eval_scale: SCALE as f32,
        steps: TrainingSteps {
            batch_size,
            batches_per_superbatch,
            start_superbatch,
            end_superbatch,
        },
        wdl_scheduler: wdl::ConstantWDL { value: wdl_value },
        lr_scheduler: lr::Warmup {
            inner: lr::CosineDecayLR {
                initial_lr,
                final_lr: initial_lr * f32::powi(0.3, 5),
                final_superbatch: end_superbatch,
            },
            warmup_batches,
        },
        save_rate,
    };

    let settings = LocalSettings {
        threads: train_threads,
        test_set: None,
        output_directory: OUTPUT_DIR,
        batch_queue_size: 64,
    };

    trainer.run(&schedule, &settings, &loader);

    if let Some(ref path) = checkpoint_save {
        trainer.save_to_checkpoint(path);
        println!("Saved checkpoint to {}", path);
    }

    for fen in [
        "r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 1",
        "4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 0 1",
        "r3qbrk/6p1/2b2pPp/p3pP1Q/PpPpP2P/3P1B2/2PB3K/R5R1 w - - 0 1",
        "8/8/1p2k1p1/3p3p/1p1P1P1P/1P2PK2/8/8 w - - 0 1",
    ] {
        println!("eval {} cp: {fen}", (SCALE as f32 * trainer.eval(fen)).round());
    }
}