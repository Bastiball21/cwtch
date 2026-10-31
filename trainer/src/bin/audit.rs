use std::fs::File;
use std::io::{BufReader, Read};
use bullet_lib::value::loader::sfbinpack::TrainingDataEntry;
use sfbinpack::{ChunkReader, read_chunk_into};

fn main() {
    println!("=== AUDIT SF BINPACK ===");
    let path = "/mnt/d/Bulk-Data/dfrc_n5000.relabel-BT4-tf13tune.binpack";
    if let Ok(file) = File::open(path) {
        let mut reader = BufReader::new(file);
        let mut buf = Vec::new();
        if read_chunk_into(&mut reader, &mut buf).unwrap() {
            let mut cr = ChunkReader::default();
            let mut count = 0;
            let mut white_matches = 0;
            let mut black_matches = 0;
            let mut total_white = 0;
            let mut total_black = 0;

            while cr.has_next(&buf) && count < 1000 {
                let entry: TrainingDataEntry = cr.next(&buf);
                count += 1;
                let stm = entry.pos.side_to_move().ordinal();
                let score = entry.score;
                let result = entry.result;

                if count <= 10 {
                    println!(
                        "Entry {}: stm={} ply={} score={} result={} fen={}",
                        count,
                        stm,
                        entry.ply,
                        score,
                        result,
                        entry.pos.fen().unwrap_or_default()
                    );
                }

                // Check correlation between score sign and result:
                // If STM is winning (score > 100), is result > 0?
                if stm == 0 {
                    total_white += 1;
                    if (score > 100 && result > 0) || (score < -100 && result < 0) || (score.abs() <= 50 && result == 0) {
                        white_matches += 1;
                    }
                } else {
                    total_black += 1;
                    if (score > 100 && result > 0) || (score < -100 && result < 0) || (score.abs() <= 50 && result == 0) {
                        black_matches += 1;
                    }
                }
            }
            println!(
                "First 1000 entries: White agree={}/{} ({:.1}%), Black agree={}/{} ({:.1}%)",
                white_matches, total_white, 100.0 * white_matches as f64 / total_white.max(1) as f64,
                black_matches, total_black, 100.0 * black_matches as f64 / total_black.max(1) as f64
            );
        }
    } else {
        println!("Could not open {}", path);
    }

    println!("\n=== AUDIT VIRIFORMAT ===");
    let vf_path = "/mnt/d/Bulk-Data/data10044405572578828463.vf.zst";
    if let Ok(file) = File::open(vf_path) {
        let mut decoder = zstd::stream::read::Decoder::new(file).unwrap();
        let mut header = [0u8; 35];
        let mut games = 0;
        let mut scored_pos = 0;
        let mut score_hist = [0usize; 5]; // < -500, -500..0, 0..500, > 500, sentinel

        while decoder.read_exact(&mut header).is_ok() && games < 50 {
            games += 1;
            let wdl = header[32]; // 0=black win, 1=draw, 2=white win
            let count = u16::from_le_bytes([header[33], header[34]]);
            if games <= 3 {
                println!("VF Game {}: wdl={} count={}", games, wdl, count);
            }

            for p in 0..count {
                let mut rec = [0u8; 4];
                if decoder.read_exact(&mut rec).is_err() { break; }
                let score = i16::from_le_bytes([rec[2], rec[3]]);
                if score == i16::MIN {
                    score_hist[4] += 1;
                } else {
                    scored_pos += 1;
                    if score < -500 { score_hist[0] += 1; }
                    else if score < 0 { score_hist[1] += 1; }
                    else if score <= 500 { score_hist[2] += 1; }
                    else { score_hist[3] += 1; }

                    if games <= 3 && p < 15 {
                        println!("  Ply {}: score={}", p, score);
                    }
                }
            }
        }
        println!("VF 50 games: {} scored positions, {} sentinel", scored_pos, score_hist[4]);
        println!("Scores: <-500: {}, -500..0: {}, 0..500: {}, >500: {}",
            score_hist[0], score_hist[1], score_hist[2], score_hist[3]);
    }
}
