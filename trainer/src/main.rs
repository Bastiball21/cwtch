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
use bullet_lib::value::loader::sfbinpack::{Color, MoveType, PieceType, TrainingDataEntry};
use bullet_trainer::reader::DataReader;
use rand::seq::SliceRandom;
use rand::{Rng, thread_rng};
use sfbinpack::{ChunkReader, read_chunk_into};
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::sync::{Arc, Mutex};
use std::thread;

// ---------------------------------------------------------------------------
// .vf.zst decoder (packed board + move replay)
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct PackedBoard {
    pieces: [u8; 64],
    stm: u8,
}

const EMPTY: u8 = 12;

impl PackedBoard {
    fn from_bytes(data: &[u8; 32]) -> Self {
        let occ = u64::from_le_bytes(data[0..8].try_into().unwrap());
        let mut pieces = [EMPTY; 64];
        let nibbles = &data[8..24];
        let mut idx = 0;
        let mut occ_copy = occ;
        while occ_copy != 0 && idx < 32 {
            let sq = occ_copy.trailing_zeros() as usize;
            occ_copy &= occ_copy - 1;
            let byte = nibbles[idx / 2];
            let nibble = if idx % 2 == 0 { byte & 0x0F } else { byte >> 4 };
            let color = (nibble >> 3) & 1;
            let piece_type = nibble & 7;
            let piece = piece_type + color * 6;
            let piece = if piece_type == 6 { 3 + color * 6 } else { piece };
            pieces[sq] = piece;
            idx += 1;
        }
        let stm = (data[24] >> 7) & 1;
        PackedBoard { pieces, stm }
    }

    fn to_chessboard(&self, score: i16, result: f32) -> Option<ChessBoard> {
        let mut bbs = [0u64; 8];
        let mut white = 0u64;
        let mut black = 0u64;

        for sq in 0..64 {
            let p = self.pieces[sq];
            if p == EMPTY {
                continue;
            }
            let bit = 1u64 << sq;
            if p / 6 == 0 {
                white |= bit;
            } else {
                black |= bit;
            }
            bbs[2 + (p % 6) as usize] |= bit;
        }
        bbs[0] = white;
        bbs[1] = black;

        if (bbs[0] & bbs[7]).count_ones() != 1 || (bbs[1] & bbs[7]).count_ones() != 1 {
            return None;
        }

        ChessBoard::from_raw(bbs, self.stm as usize, score, result).ok()
    }

    /// Best move is a capture, en passant or promotion. These positions are
    /// dropped from training: the score reflects a tactic the static eval can't see.
    fn is_tactical(&self, mv: u16) -> bool {
        let to = ((mv >> 6) & 0x3F) as usize;
        match (mv >> 14) & 0x3 {
            0 => self.pieces[to] != EMPTY,
            2 => false, // castling encodes the rook's square in `to`
            _ => true,
        }
    }

    /// Side to move is in check. Current datagen never records these, but files
    /// from older datagen builds can contain them.
    fn in_check(&self) -> bool {
        let us = self.stm;
        let them = us ^ 1;
        let king = 5 + us * 6;
        let Some(ksq) = self.pieces.iter().position(|&p| p == king) else {
            return false;
        };
        let (kr, kf) = ((ksq / 8) as i32, (ksq % 8) as i32);
        let at = |r: i32, f: i32| -> Option<u8> {
            ((0..8).contains(&r) && (0..8).contains(&f)).then(|| self.pieces[(r * 8 + f) as usize])
        };
        let enemy = |t: u8| t + them * 6;

        let pawn_rank = if us == 0 { kr + 1 } else { kr - 1 };
        for f in [kf - 1, kf + 1] {
            if at(pawn_rank, f) == Some(enemy(0)) {
                return true;
            }
        }
        const KNIGHT: [(i32, i32); 8] = [(1, 2), (2, 1), (-1, 2), (-2, 1), (1, -2), (2, -1), (-1, -2), (-2, -1)];
        for (dr, df) in KNIGHT {
            if at(kr + dr, kf + df) == Some(enemy(1)) {
                return true;
            }
        }
        const DIRS: [(i32, i32); 8] = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)];
        for (i, (dr, df)) in DIRS.iter().enumerate() {
            let slider = if i < 4 { enemy(3) } else { enemy(2) };
            let (mut r, mut f) = (kr + dr, kf + df);
            let mut dist = 1;
            while let Some(p) = at(r, f) {
                if p != EMPTY {
                    if p == slider || p == enemy(4) || (dist == 1 && p == enemy(5)) {
                        return true;
                    }
                    break;
                }
                r += dr;
                f += df;
                dist += 1;
            }
        }
        false
    }

    fn apply_move(&mut self, mv: u16) {
        let from = (mv & 0x3F) as usize;
        let to = ((mv >> 6) & 0x3F) as usize;
        let mtype = (mv >> 14) & 0x3;
        let promo = ((mv >> 12) & 0x3) as u8;

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
                let (king_to, rook_to) = match (to > from, self.stm) {
                    (true, 0) => (6, 5),
                    (true, _) => (62, 61),
                    (false, 0) => (2, 3),
                    (false, _) => (58, 59),
                };
                let kp = self.pieces[from];
                let rp = self.pieces[to];
                self.pieces[from] = EMPTY;
                self.pieces[to] = EMPTY;
                self.pieces[king_to] = kp;
                self.pieces[rook_to] = rp;
            }
            _ => {
                self.pieces[to] = 1 + promo + self.stm * 6;
                self.pieces[from] = EMPTY;
            }
        }
        self.stm ^= 1;
    }
}

// ---------------------------------------------------------------------------
// Filtering and conversion
// ---------------------------------------------------------------------------

/// What a source decided about the positions it looked at.
#[derive(Clone, Copy, Default)]
struct SourceCounts {
    seen: u64,
    kept: u64,
}

/// Pushes every position of one self-play game that survives the training filter.
/// Returns Ok(false) at a clean end of file.
fn read_vf_game<R: Read>(
    reader: &mut R,
    records: &mut Vec<u8>,
    score_filter: i32,
    counts: &mut SourceCounts,
    mut emit: impl FnMut(&PackedBoard, i16, f32),
) -> Result<bool, String> {
    let mut header = [0u8; 35];
    if reader.read_exact(&mut header).is_err() {
        return Ok(false);
    }
    let mut board = PackedBoard::from_bytes(&header[0..32].try_into().unwrap());
    let count = u16::from_le_bytes([header[33], header[34]]) as usize;
    if count > 1024 {
        return Err(format!("record count {count} too large"));
    }
    let result = match header[32] {
        0 => 0.0,
        2 => 1.0,
        _ => 0.5,
    };

    records.resize(count * 4, 0);
    reader.read_exact(records).map_err(|_| "truncated mid-game".to_string())?;

    for rec in records.chunks_exact(4) {
        let mv = u16::from_le_bytes([rec[0], rec[1]]);
        let score = i16::from_le_bytes([rec[2], rec[3]]);

        if score != i16::MIN {
            counts.seen += 1;
            if (score as i32).abs() <= score_filter && !board.is_tactical(mv) && !board.in_check() {
                emit(&board, score, result);
                counts.kept += 1;
            }
        }
        board.apply_move(mv);
    }
    Ok(true)
}

fn sf_filter(entry: &TrainingDataEntry, score_filter: i32) -> bool {
    entry.ply >= 16
        && (entry.score as i32).abs() <= score_filter
        && entry.mv.mtype() == MoveType::Normal
        && entry.pos.piece_at(entry.mv.to()).piece_type() == PieceType::None
        && !entry.pos.is_checked(entry.pos.side_to_move())
}

fn convert_sf_entry(entry: &TrainingDataEntry, score_mult: f32) -> Option<ChessBoard> {
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

    // binpack score and result are stm-relative, from_raw wants white-relative
    let mut score = (entry.score as f32 * score_mult).round().clamp(-32000.0, 32000.0) as i16;
    let mut result = f32::from(1 + entry.result) / 2.0;
    if stm > 0 {
        score = -score;
        result = 1.0 - result;
    }

    ChessBoard::from_raw(bbs, stm, score, result).ok()
}

/// Positions a binpack stream at a random chunk, so runs that never finish a
/// whole file still sample all of it instead of always reading the beginning.
fn seek_random_chunk(file: &mut BufReader<File>) -> std::io::Result<()> {
    const WINDOW: usize = 4 << 20;
    let len = file.seek(SeekFrom::End(0))?;
    if len <= 2 * WINDOW as u64 {
        file.seek(SeekFrom::Start(0))?;
        return Ok(());
    }
    let offset = thread_rng().gen_range(0..len - 2 * WINDOW as u64);
    file.seek(SeekFrom::Start(offset))?;
    let mut window = vec![0u8; WINDOW];
    file.read_exact(&mut window)?;

    for p in 0..WINDOW - 8 {
        if &window[p..p + 4] != b"BINP" {
            continue;
        }
        let size = u32::from_le_bytes(window[p + 4..p + 8].try_into().unwrap()) as usize;
        let next = p + 8 + size;
        // A chunk header followed by another chunk header cannot be a chance match
        if next + 4 <= WINDOW && &window[next..next + 4] == b"BINP" {
            file.seek(SeekFrom::Start(offset + p as u64))?;
            return Ok(());
        }
    }
    file.seek(SeekFrom::Start(0))?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Streaming loader
// ---------------------------------------------------------------------------

/// Deals files out in shuffled passes, so every file is read once per pass and
/// no position is favoured by the size of the file it lives in.
struct FileQueue {
    files: Vec<String>,
    next: usize,
    passes: u64,
    label: &'static str,
}

impl FileQueue {
    fn new(mut files: Vec<String>, label: &'static str) -> Arc<Mutex<Self>> {
        files.shuffle(&mut thread_rng());
        Arc::new(Mutex::new(Self { files, next: 0, passes: 0, label }))
    }

    fn take(queue: &Mutex<Self>) -> String {
        let mut q = queue.lock().unwrap();
        if q.next == q.files.len() {
            q.files.shuffle(&mut thread_rng());
            q.next = 0;
            q.passes += 1;
            eprintln!("[loader] {} completed pass {}", q.label, q.passes);
        }
        q.next += 1;
        q.files[q.next - 1].clone()
    }
}

type VfStream = BufReader<zstd::stream::read::Decoder<'static, BufReader<File>>>;

fn open_vf(queue: &Mutex<FileQueue>) -> VfStream {
    // A datagen file still being written or a damaged one must not stop training
    loop {
        let path = FileQueue::take(queue);
        match File::open(&path).and_then(zstd::stream::read::Decoder::new) {
            Ok(decoder) => return BufReader::with_capacity(1 << 20, decoder),
            Err(e) => {
                eprintln!("warn: {path}: {e}");
                thread::sleep(std::time::Duration::from_millis(50));
            }
        }
    }
}

fn open_sf(queue: &Mutex<FileQueue>, random_start: bool) -> BufReader<File> {
    loop {
        let path = FileQueue::take(queue);
        match File::open(&path) {
            Ok(file) => {
                let mut reader = BufReader::with_capacity(1 << 21, file);
                if random_start {
                    if let Err(e) = seek_random_chunk(&mut reader) {
                        eprintln!("warn: {path}: {e}");
                        continue;
                    }
                }
                return reader;
            }
            Err(e) => {
                eprintln!("warn: {path}: {e}");
                thread::sleep(std::time::Duration::from_millis(50));
            }
        }
    }
}

const BLOCK_BOARDS: usize = 1 << 16;

/// Reads one game at a time from each of its open files in turn, so a block of
/// output already contains many files and many games.
fn vf_worker(queue: Arc<Mutex<FileQueue>>, streams: usize, score_filter: i32, tx: SyncSender<Vec<ChessBoard>>) {
    let mut open: Vec<VfStream> = (0..streams).map(|_| open_vf(&queue)).collect();
    let mut block = Vec::with_capacity(BLOCK_BOARDS + 1024);
    let mut records = Vec::new();
    let mut counts = SourceCounts::default();

    loop {
        for stream in open.iter_mut() {
            let emit = |board: &PackedBoard, score: i16, result: f32| {
                if let Some(cb) = board.to_chessboard(score, result) {
                    block.push(cb);
                }
            };
            match read_vf_game(stream, &mut records, score_filter, &mut counts, emit) {
                Ok(true) => {}
                Ok(false) => *stream = open_vf(&queue),
                Err(e) => {
                    eprintln!("warn: skipping rest of a .vf.zst file: {e}");
                    *stream = open_vf(&queue);
                }
            }
            if block.len() >= BLOCK_BOARDS {
                let full = std::mem::replace(&mut block, Vec::with_capacity(BLOCK_BOARDS + 1024));
                if tx.send(full).is_err() {
                    return;
                }
            }
        }
    }
}

/// Reads one chunk at a time from each of its open binpacks in turn.
fn sf_worker(
    queue: Arc<Mutex<FileQueue>>,
    streams: usize,
    score_filter: i32,
    score_mult: f32,
    tx: SyncSender<Vec<ChessBoard>>,
) {
    let mut open: Vec<BufReader<File>> = (0..streams).map(|_| open_sf(&queue, true)).collect();
    let mut block = Vec::with_capacity(BLOCK_BOARDS + (1 << 20));
    let mut chunk = Vec::new();

    loop {
        for stream in open.iter_mut() {
            chunk.clear();
            match read_chunk_into(stream, &mut chunk) {
                Ok(true) => {
                    let mut reader = ChunkReader::default();
                    while reader.has_next(&chunk) {
                        let entry = reader.next(&chunk);
                        if sf_filter(&entry, score_filter) {
                            if let Some(cb) = convert_sf_entry(&entry, score_mult) {
                                block.push(cb);
                            }
                        }
                    }
                }
                Ok(false) => *stream = open_sf(&queue, false),
                Err(e) => {
                    eprintln!("warn: skipping rest of a .binpack file: {e:?}");
                    *stream = open_sf(&queue, false);
                }
            }
            if block.len() >= BLOCK_BOARDS {
                let full = std::mem::replace(&mut block, Vec::with_capacity(BLOCK_BOARDS + (1 << 20)));
                if tx.send(full).is_err() {
                    return;
                }
            }
        }
    }
}

#[derive(Clone)]
pub struct MixedLoader {
    vf_files: Vec<String>,
    sf_files: Vec<String>,
    buffer_boards: usize,
    vf_fraction: f64,
    vf_workers: usize,
    vf_streams_per_worker: usize,
    sf_workers: usize,
    score_filter: i32,
    sf_score_mult: f32,
}

impl MixedLoader {
    fn spawn_workers(&self) -> (Option<Receiver<Vec<ChessBoard>>>, Option<Receiver<Vec<ChessBoard>>>) {
        let vf_rx = (self.vf_fraction > 0.0 && !self.vf_files.is_empty()).then(|| {
            let (tx, rx) = sync_channel(self.vf_workers * 2);
            let queue = FileQueue::new(self.vf_files.clone(), "self-play");
            for _ in 0..self.vf_workers {
                let (queue, tx) = (queue.clone(), tx.clone());
                let (streams, filter) = (self.vf_streams_per_worker, self.score_filter);
                thread::spawn(move || vf_worker(queue, streams, filter, tx));
            }
            rx
        });

        let sf_rx = (self.vf_fraction < 1.0 && !self.sf_files.is_empty()).then(|| {
            let (tx, rx) = sync_channel(self.sf_workers * 2);
            let queue = FileQueue::new(self.sf_files.clone(), "binpack");
            let per_worker = self.sf_files.len().div_ceil(self.sf_workers);
            for _ in 0..self.sf_workers {
                let (queue, tx) = (queue.clone(), tx.clone());
                let (filter, mult) = (self.score_filter, self.sf_score_mult);
                thread::spawn(move || sf_worker(queue, per_worker, filter, mult, tx));
            }
            rx
        });

        (vf_rx, sf_rx)
    }

    /// Fills shuffle buffers holding exactly the requested mix of sources.
    fn assemble(&self, out: SyncSender<Vec<ChessBoard>>) {
        let (vf_rx, sf_rx) = self.spawn_workers();
        let vf_share = match (&vf_rx, &sf_rx) {
            (Some(_), Some(_)) => self.vf_fraction,
            (Some(_), None) => 1.0,
            (None, _) => 0.0,
        };
        let vf_quota = (self.buffer_boards as f64 * vf_share) as usize;
        let sf_quota = self.buffer_boards - vf_quota;

        loop {
            let mut buffer = Vec::with_capacity(self.buffer_boards + 2 * BLOCK_BOARDS + (1 << 21));
            let (mut vf_taken, mut sf_taken) = (0usize, 0usize);

            while vf_taken < vf_quota || sf_taken < sf_quota {
                if let Some(rx) = vf_rx.as_ref().filter(|_| vf_taken < vf_quota) {
                    let Ok(block) = rx.recv() else { return };
                    vf_taken += block.len();
                    buffer.extend_from_slice(&block);
                }
                if let Some(rx) = sf_rx.as_ref().filter(|_| sf_taken < sf_quota) {
                    let Ok(block) = rx.recv() else { return };
                    sf_taken += block.len();
                    buffer.extend_from_slice(&block);
                }
            }

            buffer.shuffle(&mut thread_rng());
            if out.send(buffer).is_err() {
                return;
            }
        }
    }
}

impl DataReader<ChessBoard> for MixedLoader {
    fn read_chunks<F: FnMut(&[ChessBoard]) -> bool>(&self, _: usize, mut f: F) {
        // Next buffer is prepared while the current one trains, so a shuffle of
        // several GB never stalls the GPU. Rendezvous channel: at most two buffers
        // exist at once.
        let (tx, rx) = sync_channel(0);
        let loader = self.clone();
        thread::spawn(move || loader.assemble(tx));

        for buffer in rx.iter() {
            if f(&buffer) {
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// DATA_STATS: score scale check per source, run instead of training
// ---------------------------------------------------------------------------

const STAT_BINS: usize = 801; // 10 cp bins over [-4000, 4000]

#[derive(Clone)]
struct ScaleStats {
    count: Vec<f64>,
    wins: Vec<f64>,
    positions: u64,
    draws: f64,
    abs_score: f64,
}

impl ScaleStats {
    fn new() -> Self {
        Self { count: vec![0.0; STAT_BINS], wins: vec![0.0; STAT_BINS], positions: 0, draws: 0.0, abs_score: 0.0 }
    }

    /// Score and result from the same point of view.
    fn add(&mut self, score: i32, result: f64) {
        self.positions += 1;
        self.abs_score += score.abs() as f64;
        if result == 0.5 {
            self.draws += 1.0;
        }
        if score.abs() <= 4000 {
            let bin = ((score + 4000) / 10) as usize;
            self.count[bin] += 1.0;
            self.wins[bin] += result;
        }
    }

    fn loss(&self, scale: f64) -> f64 {
        let mut total = 0.0;
        for bin in 0..STAT_BINS {
            if self.count[bin] > 0.0 {
                let score = bin as f64 * 10.0 - 4000.0;
                let p = 1.0 / (1.0 + (-score / scale).exp());
                let mean = self.wins[bin] / self.count[bin];
                total += self.count[bin] * (p - mean) * (p - mean);
            }
        }
        total
    }

    /// Scale s where sigmoid(score / s) best predicts the game result.
    fn fit_scale(&self) -> f64 {
        let (mut lo, mut hi) = (20.0f64, 3000.0f64);
        for _ in 0..80 {
            let a = lo + (hi - lo) / 3.0;
            let b = hi - (hi - lo) / 3.0;
            if self.loss(a) < self.loss(b) {
                hi = b;
            } else {
                lo = a;
            }
        }
        (lo + hi) / 2.0
    }

    fn mean_result_at(&self, cp: i32) -> f64 {
        let (mut wins, mut count) = (0.0, 0.0);
        for offset in -3..=3 {
            let bin = ((cp + 4000) / 10 + offset) as usize;
            wins += self.wins[bin];
            count += self.count[bin];
        }
        if count > 0.0 { wins / count } else { f64::NAN }
    }

    fn report(&self, name: &str, seen: u64, kept: u64) {
        let n = self.positions.max(1) as f64;
        println!(
            "{name:<72} kept {:>5.1}%  scale {:>6.1}  draws {:>4.1}%  mean|cp| {:>5.0}  win@100 {:.3}  @200 {:.3}  @400 {:.3}",
            100.0 * kept as f64 / seen.max(1) as f64,
            self.fit_scale(),
            100.0 * self.draws / n,
            self.abs_score / n,
            self.mean_result_at(100),
            self.mean_result_at(200),
            self.mean_result_at(400),
        );
    }
}

fn run_data_stats(vf_files: &[String], sf_files: &[String], per_source: u64, score_filter: i32) {
    println!("Fitting score scale on up to {per_source} kept positions per source (trainer uses scale {SCALE})");

    let mut all_vf = ScaleStats::new();
    let (mut seen, mut kept) = (0u64, 0u64);
    let mut shuffled = vf_files.to_vec();
    shuffled.shuffle(&mut thread_rng());
    for path in &shuffled {
        if all_vf.positions >= per_source {
            break;
        }
        let Ok(file) = File::open(path) else { continue };
        let Ok(decoder) = zstd::stream::read::Decoder::new(file) else { continue };
        let mut reader = BufReader::with_capacity(1 << 20, decoder);
        let (mut records, mut counts) = (Vec::new(), SourceCounts::default());
        // Sample games across each file rather than draining one file
        for _ in 0..3000 {
            let emit = |_: &PackedBoard, score: i16, result: f32| all_vf.add(score as i32, result as f64);
            match read_vf_game(&mut reader, &mut records, score_filter, &mut counts, emit) {
                Ok(true) => {}
                _ => break,
            }
        }
        seen += counts.seen;
        kept += counts.kept;
    }
    if !vf_files.is_empty() {
        all_vf.report("[all .vf.zst self-play]", seen, kept);
    }

    let mut all_sf = ScaleStats::new();
    let (mut total_seen, mut total_kept) = (0u64, 0u64);
    for path in sf_files {
        let name = Path::new(path).file_name().and_then(|s| s.to_str()).unwrap_or(path);
        let Ok(file) = File::open(path) else { continue };
        let mut reader = BufReader::with_capacity(1 << 21, file);
        let mut stats = ScaleStats::new();
        let (mut seen, mut kept, mut chunks) = (0u64, 0u64, 0u64);
        let mut chunk = Vec::new();
        // Several random spots per file, not just one run of games
        'spots: for _ in 0..8 {
            if seek_random_chunk(&mut reader).is_err() {
                break;
            }
            for _ in 0..4 {
                chunk.clear();
                match read_chunk_into(&mut reader, &mut chunk) {
                    Ok(true) => {}
                    _ => break,
                }
                chunks += 1;
                let mut entries = ChunkReader::default();
                while entries.has_next(&chunk) {
                    let entry = entries.next(&chunk);
                    seen += 1;
                    if sf_filter(&entry, score_filter) {
                        kept += 1;
                        let result = f64::from(1 + entry.result) / 2.0;
                        stats.add(entry.score as i32, result);
                        all_sf.add(entry.score as i32, result);
                    }
                }
                if stats.positions >= per_source {
                    break 'spots;
                }
            }
        }
        total_seen += seen;
        total_kept += kept;
        stats.report(name, seen, kept);
        println!("    positions per chunk: {:.0}", seen as f64 / chunks.max(1) as f64);
    }
    if !sf_files.is_empty() {
        all_sf.report("[all .binpack]", total_seen, total_kept);
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

const SCALE: i32 = 400;
const QA: i16 = 255;
const QB: i16 = 64;
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
    let score_filter           = get_env_i32("SCORE_FILTER", 10000);
    let buffer_mb              = get_env_usize("BUFFER_MB", 4096);
    let vf_fraction            = get_env_f32("VF_FRACTION", 0.5).clamp(0.0, 1.0) as f64;
    let sf_score_mult          = get_env_f32("SF_SCORE_MULT", 1.0);
    let vf_workers             = get_env_usize("VF_WORKERS", 6).max(1);
    let vf_streams             = get_env_usize("VF_STREAMS_PER_WORKER", 12).max(1);
    let sf_workers             = get_env_usize("SF_WORKERS", 6).max(1);
    let net_id                 = std::env::var("NET_ID").unwrap_or_else(|_| "cwtch".to_string());
    let output_dir: &'static str = Box::leak(std::env::var("OUTPUT_DIR").unwrap_or_else(|_| "nets".to_string()).into_boxed_str());
    let wdl_value              = get_env_f32("WDL_VALUE", 0.4);
    let checkpoint_load        = std::env::var("LOAD_CHECKPOINT").ok();
    let checkpoint_save        = std::env::var("SAVE_CHECKPOINT").ok();

    let num_input_buckets = get_num_buckets(&BUCKET_LAYOUT);

    let (vf_files, sf_files) = scan_data_dir(&data_path);
    println!(
        "Detected {} .vf.zst files and {} .binpack files",
        vf_files.len(), sf_files.len()
    );
    if vf_files.is_empty() && sf_files.is_empty() {
        panic!("DATA_PATH {} contains no .vf.zst or .binpack files", data_path);
    }
    if let Some(per_source) = std::env::var("DATA_STATS").ok().and_then(|v| v.parse::<u64>().ok()) {
        run_data_stats(&vf_files, &sf_files, per_source, score_filter);
        return;
    }

    let loader = MixedLoader {
        vf_files,
        sf_files,
        buffer_boards: (buffer_mb * 1024 * 1024 / std::mem::size_of::<ChessBoard>()).max(1 << 18),
        vf_fraction,
        vf_workers,
        vf_streams_per_worker: vf_streams,
        sf_workers,
        score_filter,
        sf_score_mult,
    };

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
    println!("BUFFER_MB              : {}", buffer_mb);
    println!("VF_FRACTION            : {}", vf_fraction);
    println!("SF_SCORE_MULT          : {}", sf_score_mult);
    println!("NET_ID / OUTPUT_DIR    : {} / {}", net_id, output_dir);
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
        net_id,
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
        output_directory: output_dir,
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