use bullet_lib::{
    game::{
        inputs::{ChessBucketsMirrored, get_num_buckets},
        outputs::MaterialCount,
    },
    nn::{
        InitSettings, Shape,
        optimiser::AdamW,
    },
    trainer::{
        save::SavedFormat,
    },
    value::ValueTrainerBuilder,
};

const HIDDEN_SIZE: usize = 1536;
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

fn main() {
    let num_input_buckets = get_num_buckets(&BUCKET_LAYOUT);

    let mut trainer = ValueTrainerBuilder::default()
        .dual_perspective()
        .optimiser(AdamW)
        .inputs(ChessBucketsMirrored::new(BUCKET_LAYOUT))
        .output_buckets(MaterialCount::<NUM_OUTPUT_BUCKETS>)
        .save_format(&[
            SavedFormat::id("l0w")
                .transform(move |store, weights| {
                    let factoriser = store.get("l0f").values.f32().repeat(num_input_buckets);
                    weights.into_iter().zip(factoriser).map(|(a, b)| a + b).collect()
                })
                .round()
                .quantise::<i16>(QA),
            SavedFormat::id("l0b").round().quantise::<i16>(QA),
            SavedFormat::id("l1w").round().quantise::<i16>(QB).transpose(),
            SavedFormat::id("l1b").round().quantise::<i16>(QA * QB),
        ])
        .loss_fn(|output, target| output.sigmoid().squared_error(target))
        .build(|builder, stm_inputs, ntm_inputs, output_buckets| {
            let l0f = builder.new_weights("l0f", Shape::new(HIDDEN_SIZE, 768), InitSettings::Zeroed);
            let expanded_factoriser = l0f.repeat(num_input_buckets);

            let mut l0 = builder.new_affine("l0", 768 * num_input_buckets, HIDDEN_SIZE);
            l0.weights = l0.weights + expanded_factoriser;

            let l1 = builder.new_affine("l1", 2 * HIDDEN_SIZE, NUM_OUTPUT_BUCKETS);

            let stm_hidden = l0.forward(stm_inputs).sqrrelu();
            let ntm_hidden = l0.forward(ntm_inputs).sqrrelu();
            let hidden_layer = stm_hidden.concat(ntm_hidden);
            l1.forward(hidden_layer).select(output_buckets)
        });

    println!("Loading checkpoint trainer/nets/cwtch-600...");
    trainer.load_from_checkpoint("trainer/nets/cwtch-600");

    let fens = [
        "r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 1",
        "4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 0 1",
        "r3qbrk/6p1/2b2pPp/p3pP1Q/PpPpP2P/3P1B2/2PB3K/R5R1 w - - 0 1",
        "8/8/1p2k1p1/3p3p/1p1P1P1P/1P2PK2/8/8 w - - 0 1",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    ];

    println!("\nBullet Trainer Eval vs Cwtch Engine:");
    for fen in fens {
        let cp = (SCALE as f32 * trainer.eval(fen)).round();
        println!("FEN: {fen}");
        println!("  Bullet eval: {cp} cp");
    }
}
