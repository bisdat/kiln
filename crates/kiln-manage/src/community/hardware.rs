//! What a tester shares (PLAN 3.9): the graphics card, its driver, the video memory, the system and how the game
//! runs, and from those what this PC can test that the team can't (FSR 4, XeSS, frame generation, the Steam Deck).
//! Read without new privileges; the volunteer screen shows exactly this before anything is sent.

use serde::{Deserialize, Serialize};

/// Hardware the team wants tested.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Capability {
    /// AMD RX 6000 and newer.
    Fsr4,
    /// Intel Arc.
    Xess,
    /// NVIDIA RTX 40 and 50.
    Framegen,
    /// A Steam Deck.
    Deck,
}

impl Capability {
    /// The word the screens show.
    pub fn label(self) -> &'static str {
        match self {
            Capability::Fsr4 => "FSR 4",
            Capability::Xess => "XeSS",
            Capability::Framegen => "frame generation",
            Capability::Deck => "Steam Deck",
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Hardware {
    pub gpu: String,
    pub driver: String,
    pub vram: String,
    pub os: String,
    /// How the game runs: "Windows", "GE-Proton10-3", "Proton 10.0", "Wine 10.0".
    pub runner: String,
    pub caps: Vec<Capability>,
    /// `caps` in words.
    pub covers: Vec<String>,
}

impl Hardware {
    /// What `POST /tester` sends: the lines the screen showed.
    pub fn wire(&self) -> serde_json::Value {
        serde_json::json!({ "gpu": self.gpu, "driver": self.driver, "vram": self.vram, "os": self.os, "runner": self.runner, "covers": self.caps })
    }
}
