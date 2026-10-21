use std::fs;

use webnn_graph::parser::parse_wg_text;
use webnn_graph::serialize::{serialize_graph_to_wg_text, SerializeOptions};

#[test]
fn format_reference_round_trips_through_graph_json() {
    let path = format!(
        "{}/examples/format_reference.webnn",
        env!("CARGO_MANIFEST_DIR")
    );
    let source = fs::read_to_string(path).expect("read format reference example");
    let parsed = parse_wg_text(&source).expect("parse format reference example");

    assert_eq!(parsed.version, 2);
    assert!(parsed.quantized);
    assert!(parsed.inputs["input"].static_shape().is_none());
    assert!(parsed.consts["bias"].shape.is_empty());
    assert!(parsed.consts.contains_key("inline_mask"));
    assert!(parsed.consts.contains_key("external_weight"));
    assert_eq!(
        parsed.nodes[1].outputs.as_deref(),
        Some(&["left".into(), "right".into()][..])
    );

    let serialized = serialize_graph_to_wg_text(&parsed, SerializeOptions::default())
        .expect("serialize format reference example");
    let reparsed = parse_wg_text(&serialized).expect("reparse serialized format reference example");

    assert_eq!(
        serde_json::to_value(parsed).expect("serialize original AST"),
        serde_json::to_value(reparsed).expect("serialize round-tripped AST")
    );
}
