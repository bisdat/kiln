//! The Host's paired dependency-floor matrix, through the real manifest parser and shared launcher/kilnctl resolver.
use kiln_manage::manifest::{self, RunSide};
use kiln_manage::resolver::{self, Candidate, Environment};
use serde_json::{Value, json};

fn fixture() -> Value {
    serde_json::from_str(include_str!("../../../schema/fixtures/dependency-floor.json")).unwrap()
}

fn candidate(id: &str, version: &str, extra: Value) -> Candidate {
    let mut raw = json!({"id": id, "version": version, "apiVersion": "0.2", "entryAssembly": "M.dll"});
    raw.as_object_mut().unwrap().extend(extra.as_object().unwrap().clone());
    Candidate { dir: format!("/mods/{id}"), manifest: Some(manifest::parse_kiln(&raw.to_string()).unwrap()), error: None }
}

#[test]
fn dependency_ranges_distinguish_kiln_from_mods() {
    for row in fixture()["cases"].as_array().unwrap() {
        let (range, installed) = (row[0].as_str().unwrap(), row[1].as_str().unwrap());
        for (id, accepts) in [("kiln", row[2].as_bool().unwrap()), ("lib", row[3].as_bool().unwrap())] {
            let app = candidate("app", "1.0.0", json!({"dependencies": {id: range}}));
            let lib = candidate("lib", installed, json!({}));
            // A normal dependency has the same meaning whether it is queued or already running.
            for loaded in [false, true] {
                let env = Environment {
                    kiln: if id == "kiln" { installed.into() } else { "0.2.0-dev.31".into() },
                    api: "0.2.0".into(), side: RunSide::Server, unavailable_features: vec![], hosting: false,
                    loaded: if loaded && id != "kiln" { vec![lib.manifest.clone().unwrap()] } else { vec![] },
                };
                let candidates = if id == "kiln" || loaded { vec![app.clone()] } else { vec![app.clone(), lib.clone()] };
                let result = resolver::resolve(&candidates, &env).unwrap();
                assert_eq!(result.order.iter().any(|m| m == "app"), accepts, "{id} {range:?} on {installed}, loaded={loaded}");
                if accepts {
                    assert!(result.rejected.is_empty(), "{result:?}");
                } else {
                    let text = &app.manifest.as_ref().unwrap().dependencies[0].range;
                    let reason = if id == "kiln" { format!("needs Kiln {text}; this is Kiln {installed}") }
                        else { format!("needs lib {text}, found lib {installed}") };
                    assert_eq!(result.rejected.len(), 1, "{result:?}");
                    assert_eq!(result.rejected[0].reason, reason);
                }
            }
        }
    }
}

#[test]
fn malformed_prerelease_requirements_remain_manifest_errors_for_every_dependency() {
    for range in fixture()["invalid"].as_array().unwrap() {
        for id in ["kiln", "lib"] {
            let raw = json!({"id": "app", "version": "1.0.0", "apiVersion": "0.2", "entryAssembly": "M.dll", "dependencies": {id: range}});
            let error = manifest::parse_kiln(&raw.to_string()).unwrap_err();
            assert!(error.message.contains(&format!("dependency '{id}'")), "{error}");
        }
    }
}
