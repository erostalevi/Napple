//! napple-sigs: shared signatures (schema, stores, matching, resolver, REA adapter, share flow).
//!
//! Hexagonal layout: `domain` + `validate` are pure; `repo` and `reverser` hold the ports and their
//! adapters; `resolver` and `share` are the use cases; `cli` is the interface.
pub mod cli;
pub mod domain;
pub mod exec;
pub mod repo;
pub mod resolver;
pub mod reverser;
pub mod share;
pub mod validate;

pub fn run(args: &[String]) -> Result<(), String> {
    cli::run(args)
}
