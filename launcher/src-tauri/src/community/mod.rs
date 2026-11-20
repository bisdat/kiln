//! The Discord features' parts that run beside the commands (commands/community.rs): Rich Presence's thread, the posts'
//! refresh and reply notifications, the prepared support reports and the known-problems copy, the tester's local
//! state. The logic is in kiln_manage::community.

pub mod posts;
pub mod presence;
pub mod report;
pub mod testers;
