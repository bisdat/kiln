use kfc::reflection::TypeIndex;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum MappingError {
    #[error("utf-8 conversion error: {0}")]
    Utf8(#[from] std::str::Utf8Error),

    #[error("unexpected end of data")]
    UnexpectedEndOfData,
    #[error("invalid type index: {0}")]
    InvalidTypeIndex(TypeIndex),
    #[error("invalid type hash: {0}")]
    InvalidTypeHash(u32),
    #[error("{0}")]
    UnsupportedOperation(&'static str),
    // kiln-eml: a stored array count the bytes after the array's start cannot hold (see `check_count` in mod.rs)
    #[error("array of {count} elements (element size {element_size}) at offset {offset} does not fit in {available} bytes of data")]
    ArrayCountExceedsData {
        count: usize,
        element_size: usize,
        offset: usize,
        available: usize,
    },
}
