// SPDX-License-Identifier: MIT
//! The format-1 DTO, strict loader, canonical digest, schema drift and pure query.
//! Synthetic catalogs here test mechanics only; they are never production API proof.
use serde_json::{Value, json};
use sfi_package::json::Value as Doc;
use sfi_package::package::revisions::revision as sha256_hex;
use sfi_package::parity::{
    AliasOf, AliasRelation, Catalog, CatalogError, CatalogHeader, CompositePart, ContractRevision,
    ExpectedCatalog, FamilyDigest, LiveEvidence, ObligationCounts, Outcome, ParityStatus,
    PartRevision, Presence, RevisionId, Scope, Symbol, SymbolRevision,
};

const FIXTURE: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/tests/consumer/sample-catalog-v1.json"
);
const SCHEMA: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/schema/parity-catalog-v1.schema.json"
);

fn h(seed: &str) -> String {
    sha256_hex(seed.as_bytes())
}
fn counts() -> ObligationCounts {
    ObligationCounts {
        full: 1,
        partial: 1,
        missing: 3,
        na: 2,
    }
}
fn header() -> CatalogHeader {
    CatalogHeader {
        format: "kiln-sfi-parity-catalog".into(),
        revisions: RevisionId::ALL
            .iter()
            .map(|r| ContractRevision {
                id: *r,
                source_version: r.source_version().into(),
                source_pin: "d8a394bcb180ceb5bdfce48048dcc4f2e0108bd8".into(),
                inventory_sha256: h(r.source_version()),
            })
            .collect(),
        final_manifest_sha256: h("final"),
        frozen: vec![
            FamilyDigest {
                family: "eml-pure".into(),
                sha256: h("eml-pure"),
            },
            FamilyDigest {
                family: "sf-mod-api".into(),
                sha256: h("sf-mod-api"),
            },
        ],
        policy_sha256: h("policy"),
        registry_sha256: h("registry"),
        backend_build: "sample-build".into(),
        evidence: LiveEvidence {
            scope: "aggregate".into(),
            modules: 1,
            runtime_modules: 0,
            assertions: 2,
            mappings: 2,
            sha256: h("evidence"),
        },
        symbol_obligations: counts(),
        other_obligations: counts(),
    }
}
fn rev(
    revision: RevisionId,
    presence: Presence,
    parity: ParityStatus,
    tests: &[&str],
) -> SymbolRevision {
    let required = if parity == ParityStatus::Excluded {
        0
    } else {
        2
    };
    SymbolRevision {
        revision,
        presence,
        parity,
        required,
        proven_full: match parity {
            ParityStatus::Full => 2,
            ParityStatus::Partial => 1,
            _ => 0,
        },
        tests: tests.iter().map(|t| t.to_string()).collect(),
        digest: h(&format!("{revision:?}{parity:?}")),
    }
}
fn symbol(name: &str, scope: Scope, kind: &str, revisions: Vec<SymbolRevision>) -> Symbol {
    Symbol {
        name: name.into(),
        scope,
        kind: kind.into(),
        introduced: None,
        alias_of: None,
        revisions,
        composite: Vec::new(),
    }
}
fn part(id: &str, owner: &str, milestone: &str, parity: ParityStatus) -> CompositePart {
    CompositePart {
        id: id.into(),
        owner: owner.into(),
        milestone: milestone.into(),
        revisions: RevisionId::ALL
            .iter()
            .map(|r| PartRevision {
                revision: *r,
                parity,
                digest: h(id),
            })
            .collect(),
    }
}
fn symbols() -> Vec<Symbol> {
    use ParityStatus::*;
    use Presence::*;
    use RevisionId::*;
    let mut insert = symbol(
        "table.insert",
        Scope::NamespaceMember,
        "function",
        vec![
            rev(Sf179, Callable, Partial, &["t1"]),
            rev(Sf180, Callable, Partial, &["t1"]),
        ],
    );
    insert.composite = vec![
        part("table.insert", "lua-env-lifecycle", "M1", Full),
        part(
            "table.insert[typed-resource]",
            "reflection-data",
            "M2",
            Missing,
        ),
    ];
    let mut alias = symbol(
        "runtime.types",
        Scope::NamespaceMember,
        "namespace",
        Vec::new(),
    );
    alias.alias_of = Some(AliasOf {
        target: "game.types".into(),
        relation: AliasRelation::Delegate,
    });
    let mut builtin = symbol(
        "builtin.integer",
        Scope::NamespaceMember,
        "namespace",
        vec![
            rev(Sf179, Value, Missing, &[]),
            rev(Sf180, Value, Missing, &[]),
        ],
    );
    builtin.alias_of = Some(AliasOf {
        target: "integer".into(),
        relation: AliasRelation::NamespaceIdentity,
    });
    vec![
        builtin,
        symbol(
            "game.types",
            Scope::NamespaceMember,
            "namespace",
            vec![
                rev(Sf179, Value, Missing, &[]),
                rev(Sf180, Value, Missing, &[]),
            ],
        ),
        symbol(
            "integer",
            Scope::RootGlobal,
            "field",
            vec![
                rev(Sf179, Value, Full, &["i1", "i2"]),
                rev(Sf180, Value, Full, &["i1", "i2"]),
            ],
        ),
        symbol(
            "integer.u8.neg",
            Scope::NamespaceMember,
            "function",
            vec![
                rev(Sf179, Absent, Full, &["n1"]),
                rev(Sf180, Absent, Full, &["n1"]),
            ],
        ),
        symbol(
            "loader.runtime.register_dll",
            Scope::NamespaceMember,
            "function",
            vec![rev(Sf179, NA, Excluded, &[]), rev(Sf180, NA, Excluded, &[])],
        ),
        alias,
        symbol(
            "shroudforge.new_api",
            Scope::NamespaceMember,
            "function",
            vec![
                rev(Sf179, NotInRevision, Missing, &[]),
                rev(Sf180, Callable, Missing, &[]),
            ],
        ),
        insert,
    ]
}
fn sample() -> Catalog {
    Catalog::new(header(), symbols()).unwrap()
}
/// json! as a document-ordered value. The digest covers the file's own field order, which a
/// serde_json map keeps only with preserve_order; an object of several keys is written as
/// text instead (`ordered`).
macro_rules! oj {
    ($($json:tt)+) => {
        Doc::from(json!($($json)+))
    };
}
fn ordered(text: &str) -> Doc {
    serde_json::from_str(text).unwrap()
}
fn reload(value: &Doc) -> Result<Catalog, CatalogError> {
    Catalog::from_json(&serde_json::to_vec(value).unwrap())
}
fn json_of(catalog: &Catalog) -> Doc {
    serde_json::from_slice(&catalog.to_json()).unwrap()
}
/// Re-seal a mutated body with a correct digest, so only semantic validation can refuse it.
fn resealed(mut value: Doc) -> Doc {
    let object = value.as_object_mut().unwrap();
    object.shift_remove("digest");
    let digest = sha256_hex(&serde_json::to_vec(&value).unwrap());
    value["digest"] = oj!(digest);
    value
}

#[test]
fn sample_fixture_is_the_deterministic_encoding() {
    let catalog = sample();
    assert_eq!(catalog.to_json(), sample().to_json(), "deterministic");
    let fixture = std::fs::read(FIXTURE).expect("committed sample fixture");
    assert_eq!(
        String::from_utf8(fixture.clone()).unwrap(),
        String::from_utf8(catalog.to_json()).unwrap()
    );
    let loaded = Catalog::from_json(&fixture).unwrap();
    assert_eq!(loaded, catalog);
    // The digest is SHA-256 of the canonical body without the digest field.
    let mut value = json_of(&catalog);
    let recorded = value
        .as_object_mut()
        .unwrap()
        .shift_remove("digest")
        .unwrap();
    assert_eq!(
        recorded,
        oj!(sha256_hex(&serde_json::to_vec(&value).unwrap()))
    );
    assert_eq!(
        sha256_hex(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn schema_version_is_required_and_only_one_is_supported() {
    let value = json_of(&sample());
    for (version, expected) in [
        (json!(2), "unsupported"),
        (json!(0), "unsupported"),
        (json!("1"), "missing"),
    ] {
        let mut bad = value.clone();
        bad["schema_version"] = version.into();
        let error = reload(&bad).unwrap_err().to_string();
        assert!(error.contains(expected), "{error}");
    }
    let mut missing = value.clone();
    missing
        .as_object_mut()
        .unwrap()
        .shift_remove("schema_version");
    assert!(matches!(reload(&missing), Err(CatalogError::Malformed(_))));
}

#[test]
fn malformed_duplicate_unknown_and_tampered_input_is_refused() {
    let text = String::from_utf8(sample().to_json()).unwrap();
    assert!(matches!(
        Catalog::from_json(b"{"),
        Err(CatalogError::Malformed(_))
    ));
    let duplicate = text.replacen(
        "\"schema_version\": 1,",
        "\"schema_version\": 1,\n  \"schema_version\": 1,",
        1,
    );
    assert!(
        Catalog::from_json(duplicate.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate")
    );
    let nested = text.replacen(
        "\"kind\": \"function\",",
        "\"kind\": \"function\", \"kind\": \"method\",",
        1,
    );
    assert!(
        Catalog::from_json(nested.as_bytes())
            .unwrap_err()
            .to_string()
            .contains("duplicate")
    );
    let mut unknown = json_of(&sample());
    unknown["symbols"][0]["supported"] = oj!(true);
    assert!(matches!(
        reload(&resealed(unknown)),
        Err(CatalogError::Malformed(_))
    ));
    // Format 1 requires the nullable keys: dropping one keeps the canonical digest (it
    // re-serializes as null) but is malformed under the schema.
    for field in ["introduced", "alias_of"] {
        let mut absent = json_of(&sample());
        absent["symbols"][0]
            .as_object_mut()
            .unwrap()
            .shift_remove(field);
        assert!(
            matches!(reload(&absent), Err(CatalogError::Malformed(_))),
            "{field}: {:?}",
            reload(&absent)
        );
    }
    let mut tampered = json_of(&sample());
    tampered["header"]["backend_build"] = oj!("tampered");
    assert!(matches!(
        reload(&tampered),
        Err(CatalogError::Digest { .. })
    ));
}

#[test]
fn contradictory_presence_status_and_evidence_are_refused_even_when_resealed() {
    let base = json_of(&sample());
    let index = |name: &str| {
        base["symbols"]
            .as_array()
            .unwrap()
            .iter()
            .position(|s| s["name"] == name)
            .unwrap()
    };
    type Case<'a> = (&'a str, Box<dyn Fn(&mut Doc) + 'a>);
    let cases: Vec<Case> = vec![
        (
            "n/a presence",
            Box::new(|v| {
                v["symbols"][index("loader.runtime.register_dll")]["revisions"][0]["parity"] =
                    oj!("full")
            }),
        ),
        (
            "n/a presence",
            Box::new(|v| {
                v["symbols"][index("integer")]["revisions"][0]["parity"] = oj!("excluded")
            }),
        ),
        (
            "contradictory",
            Box::new(|v| v["symbols"][index("integer")]["revisions"][0]["tests"] = oj!([])),
        ),
        (
            "contradictory",
            Box::new(|v| {
                v["symbols"][index("game.types")]["revisions"][0]["tests"] = oj!(["forged"])
            }),
        ),
        (
            "contradictory",
            Box::new(|v| v["symbols"][index("game.types")]["revisions"][0]["parity"] = oj!("full")),
        ),
        (
            "unknown presence",
            Box::new(|v| {
                let r = &mut v["symbols"][index("integer")]["revisions"][0];
                r["presence"] = oj!("unknown");
            }),
        ),
        (
            "composite Full",
            Box::new(|v| {
                let s = &mut v["symbols"][index("table.insert")];
                s["revisions"][0] = ordered(&format!(
                    r#"{{"revision":"sf179","presence":"callable","parity":"full","required":2,"proven_full":2,"tests":["t1"],"digest":"{}"}}"#,
                    h("x")
                ));
            }),
        ),
        (
            "every owner",
            Box::new(|v| {
                v["symbols"][index("table.insert")]["composite"]
                    .as_array_mut()
                    .unwrap()
                    .pop();
            }),
        ),
        (
            "one entry per observed revision",
            Box::new(|v| {
                v["symbols"][index("integer")]["revisions"]
                    .as_array_mut()
                    .unwrap()
                    .pop();
            }),
        ),
        (
            "sorted and unique",
            Box::new(|v| v["symbols"].as_array_mut().unwrap().swap(1, 2)),
        ),
        (
            "missing alias target",
            Box::new(|v| {
                v["symbols"][index("runtime.types")]["alias_of"]["target"] = oj!("game.nothing")
            }),
        ),
        (
            "pure delegate",
            Box::new(|v| v["symbols"][index("game.types")]["revisions"] = oj!([])),
        ),
        (
            "introduced",
            Box::new(|v| v["symbols"][index("integer")]["introduced"] = oj!("1.6.0")),
        ),
        (
            "revisions must be exactly",
            Box::new(|v| v["header"]["revisions"].as_array_mut().unwrap().reverse()),
        ),
        (
            "aggregate evidence",
            Box::new(|v| v["header"]["evidence"]["scope"] = oj!("m1-only")),
        ),
    ];
    for (needle, mutate) in cases {
        let mut bad = base.clone();
        mutate(&mut bad);
        let error = reload(&resealed(bad)).unwrap_err().to_string();
        assert!(error.contains(needle), "{needle}: {error}");
    }
    // A cycle: two pure delegates naming each other.
    let mut cyclic = base.clone();
    let alias = index("runtime.types");
    let mut other = cyclic["symbols"][alias].clone();
    other["name"] = oj!("runtime.types2");
    other["alias_of"]["target"] = oj!("runtime.types");
    cyclic["symbols"][alias]["alias_of"]["target"] = oj!("runtime.types2");
    cyclic["symbols"]
        .as_array_mut()
        .unwrap()
        .insert(alias + 1, other);
    assert!(
        reload(&resealed(cyclic))
            .unwrap_err()
            .to_string()
            .contains("cycle")
    );
    // A cycle between namespace identities that keep their own revisions.
    let mut identity_cycle = base.clone();
    identity_cycle["symbols"][index("integer")]["alias_of"] =
        ordered(r#"{"target": "builtin.integer", "relation": "namespace-identity"}"#);
    assert!(
        reload(&resealed(identity_cycle))
            .unwrap_err()
            .to_string()
            .contains("cycle")
    );
}

#[test]
fn expected_fingerprints_must_match_exactly() {
    let catalog = sample();
    let h0 = catalog.header();
    let expected = ExpectedCatalog {
        final_manifest_sha256: h0.final_manifest_sha256.clone(),
        frozen: h0.frozen.clone(),
        inventories: h0
            .revisions
            .iter()
            .map(|r| (r.id, r.inventory_sha256.clone()))
            .collect(),
        policy_sha256: h0.policy_sha256.clone(),
        registry_sha256: h0.registry_sha256.clone(),
        backend_build: h0.backend_build.clone(),
        evidence_sha256: Some(h0.evidence.sha256.clone()),
    };
    catalog.check_expected(&expected).unwrap();
    type Change = Box<dyn Fn(&mut ExpectedCatalog)>;
    let variants: Vec<Change> = vec![
        Box::new(|e| e.final_manifest_sha256 = h("other")),
        Box::new(|e| e.frozen[0].sha256 = h("other")),
        Box::new(|e| e.inventories[1].1 = h("other")),
        Box::new(|e| e.policy_sha256 = h("other")),
        Box::new(|e| e.registry_sha256 = h("other")),
        Box::new(|e| e.backend_build = "other".into()),
        Box::new(|e| e.evidence_sha256 = Some(h("other"))),
    ];
    for change in variants {
        let mut stale = expected.clone();
        change(&mut stale);
        assert!(matches!(
            catalog.check_expected(&stale),
            Err(CatalogError::Fingerprint(_))
        ));
    }
}

#[test]
fn query_resolves_aliases_and_keeps_presence_distinct_from_parity() {
    use RevisionId::*;
    let catalog = sample();
    let absent = catalog.query(Sf179, "integer.u8.neg", None);
    assert_eq!(
        (absent.outcome, absent.parity),
        (Outcome::Absent, Some(ParityStatus::Full))
    );
    let later = catalog.query(Sf179, "shroudforge.new_api", None);
    assert_eq!(later.outcome, Outcome::NotInRevision);
    assert_eq!(
        catalog.query(Sf180, "shroudforge.new_api", None).outcome,
        Outcome::Known
    );
    let excluded = catalog.query(Sf180, "loader.runtime.register_dll", None);
    assert_eq!(
        (excluded.outcome, excluded.parity),
        (Outcome::Excluded, Some(ParityStatus::Excluded))
    );
    let unknown = catalog.query(Sf180, "game.nothing", None);
    assert_eq!((unknown.outcome, unknown.parity), (Outcome::Unknown, None));
    let delegate = catalog.query(Sf180, "runtime.types", None);
    assert_eq!(delegate.resolved.as_deref(), Some("game.types"));
    assert_eq!(
        delegate.via,
        vec![("runtime.types".to_owned(), AliasRelation::Delegate)]
    );
    let identity = catalog.query(Sf180, "builtin.integer", None);
    assert_eq!(
        identity.resolved.as_deref(),
        Some("builtin.integer"),
        "identity keeps its own obligation"
    );
    assert_eq!(
        identity.parity,
        Some(ParityStatus::Missing),
        "no child behaviour from the alias"
    );
    assert_eq!(
        identity.via,
        vec![(
            "builtin.integer".to_owned(),
            AliasRelation::NamespaceIdentity
        )]
    );
    assert_eq!(
        catalog
            .query(Sf180, "integer", Some(Scope::UserdataMember))
            .outcome,
        Outcome::Unknown
    );
    let inherited = catalog.query(Sf179, "builtin.integer.u8.neg", None);
    assert_eq!(inherited.resolved.as_deref(), Some("integer.u8.neg"));
    assert_eq!(
        (inherited.outcome, inherited.parity),
        (Outcome::Absent, Some(ParityStatus::Full))
    );
    assert_eq!(
        inherited.via,
        vec![(
            "builtin.integer".to_owned(),
            AliasRelation::NamespaceIdentity
        )]
    );
    assert_eq!(
        catalog
            .query(Sf179, "builtin.integer.u8.nothing", None)
            .outcome,
        Outcome::Unknown
    );
    let composite = catalog.query(Sf180, "table.insert", Some(Scope::NamespaceMember));
    assert_eq!(
        composite.parity,
        Some(ParityStatus::Partial),
        "typed part missing keeps the parent partial"
    );
}

/// The checked-in schema and the Rust DTO cannot drift: every enum and every object's
/// field set equals what format 1 serializes.
#[test]
fn checked_in_schema_matches_the_dto() {
    let schema: Value = serde_json::from_str(&std::fs::read_to_string(SCHEMA).unwrap()).unwrap();
    let defs = &schema["$defs"];
    assert_eq!(schema["properties"]["schema_version"]["const"], 1);
    let enum_of = |name: &str| defs[name]["enum"].clone();
    let ser = |v: Vec<Value>| Value::Array(v);
    assert_eq!(
        enum_of("revision_id"),
        ser(RevisionId::ALL.iter().map(|r| json!(r)).collect())
    );
    use Presence as P;
    assert_eq!(
        enum_of("presence"),
        ser([
            P::Callable,
            P::Value,
            P::Declaration,
            P::Absent,
            P::NotInRevision,
            P::NA,
            P::Unknown
        ]
        .iter()
        .map(|p| json!(p))
        .collect())
    );
    use ParityStatus::*;
    assert_eq!(
        enum_of("parity"),
        ser([Full, Partial, Missing, Excluded]
            .iter()
            .map(|p| json!(p))
            .collect())
    );
    use Scope::*;
    assert_eq!(
        enum_of("scope"),
        ser([
            RootGlobal,
            NamespaceMember,
            UserdataMember,
            ReturnedField,
            TypeDeclaration,
            ContractData
        ]
        .iter()
        .map(|s| json!(s))
        .collect())
    );
    assert_eq!(
        enum_of("alias_relation"),
        json!(["delegate", "namespace-identity"])
    );
    let keys = |v: &Value| -> Vec<String> {
        let mut k: Vec<_> = v.as_object().unwrap().keys().cloned().collect();
        k.sort();
        k
    };
    let required = |v: &Value| -> Vec<String> {
        let mut k: Vec<String> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap().into())
            .collect();
        k.sort();
        assert_eq!(k, keys(&v["properties"]), "required == properties");
        k
    };
    let file: Value = serde_json::from_slice(&sample().to_json()).unwrap();
    assert_eq!(required(&schema), keys(&file));
    assert_eq!(required(&defs["header"]), keys(&file["header"]));
    assert_eq!(
        required(&defs["header"]["properties"]["evidence"]),
        keys(&file["header"]["evidence"])
    );
    assert_eq!(
        required(&defs["counts"]),
        keys(&file["header"]["symbol_obligations"])
    );
    assert_eq!(
        required(&defs["header"]["properties"]["revisions"]["items"]),
        keys(&file["header"]["revisions"][0])
    );
    assert_eq!(
        required(&defs["header"]["properties"]["frozen"]["items"]),
        keys(&file["header"]["frozen"][0])
    );
    let aliased = file["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .find(|s| !s["alias_of"].is_null())
        .unwrap();
    assert_eq!(
        required(&defs["symbol"]["properties"]["alias_of"]["oneOf"][1]),
        keys(&aliased["alias_of"])
    );
    let insert = file["symbols"]
        .as_array()
        .unwrap()
        .iter()
        .find(|s| s["name"] == "table.insert")
        .unwrap();
    assert_eq!(required(&defs["symbol"]), keys(insert));
    assert_eq!(
        required(&defs["symbol_revision"]),
        keys(&insert["revisions"][0])
    );
    assert_eq!(
        required(&defs["composite_part"]),
        keys(&insert["composite"][0])
    );
    assert_eq!(
        required(&defs["composite_part"]["properties"]["revisions"]["items"]),
        keys(&insert["composite"][0]["revisions"][0])
    );
}

/// Explicit fixture refresh: `cargo test -p sfi-package --test parity_catalog -- --ignored`.
#[test]
#[ignore = "explicit reviewed refresh of the synthetic sample fixture"]
fn write_sample_fixture() {
    std::fs::write(FIXTURE, sample().to_json()).unwrap();
}
