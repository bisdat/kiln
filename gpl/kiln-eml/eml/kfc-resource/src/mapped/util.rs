use std::borrow::Borrow;

use kfc::reflection::TypeRegistry;
use kfc::reflection::PrimitiveType;

use super::{TypeHandle, MappingError};

/// kiln-eml: refuses an array whose stored count the bytes from its start cannot hold. kfc trusted the count, and every
/// reader of a mapped array makes something per element up front (kfc's `Value::from` reserves the count whole; EML's
/// and ShroudForge's Lua wrappers and Kiln's value graph make one lazy entry per element), so a count of 2^32-1 in a
/// few bytes asked for about 100 GiB at once and aborted the process. Each element takes `element_size` bytes (its
/// stride; kfc's writer and the game fill every element to its full size), so valid data always passes; a count of 0
/// reads nothing and always passes. A missing element type is left to `MappedArray::new`, which panics on it as before.
#[inline]
pub fn check_count<T>(
    r#type: &TypeHandle<T>,
    data: &[u8],
    offset: usize,
    count: usize,
) -> Result<(), MappingError>
where
    T: Borrow<TypeRegistry> + Clone,
{
    if count == 0 {
        return Ok(());
    }
    let Some(element) = get_inner_type_opt(r#type) else {
        return Ok(());
    };
    let element_size = element_size(&element);
    let fits = data.len().checked_sub(offset).is_some_and(|rest| count <= rest / element_size);
    if fits {
        Ok(())
    } else {
        Err(MappingError::ArrayCountExceedsData { count, element_size, offset, available: data.len() })
    }
}

/// kiln-eml: the fewest bytes one array element of `element` takes: its registered size, but never less than its
/// kind reads (a faulty registry can register a size of 0) and never 0 (a count of zero-sized elements would cost
/// nothing to claim). In the game's registry every type is at least 1 byte and at least what its kind reads.
#[inline]
pub fn element_size<T>(element: &TypeHandle<T>) -> usize
where
    T: Borrow<TypeRegistry> + Clone,
{
    let read = match element.primitive_type {
        PrimitiveType::Bool | PrimitiveType::UInt8 | PrimitiveType::SInt8 | PrimitiveType::Bitmask8 => 1,
        PrimitiveType::UInt16 | PrimitiveType::SInt16 | PrimitiveType::Bitmask16 => 2,
        PrimitiveType::UInt32 | PrimitiveType::SInt32 | PrimitiveType::Float32 | PrimitiveType::Bitmask32 => 4,
        PrimitiveType::UInt64 | PrimitiveType::SInt64 | PrimitiveType::Float64 | PrimitiveType::Bitmask64 => 8,
        // the value's own integer type (not unwrapped: a typedef there is a faulty registry, and its read fails anyway)
        PrimitiveType::Enum => match element.inner_type.and_then(|inner| element.type_registry().borrow().get(inner)).map(|inner| inner.primitive_type) {
            Some(PrimitiveType::UInt16 | PrimitiveType::SInt16) => 2,
            Some(PrimitiveType::UInt32 | PrimitiveType::SInt32) => 4,
            Some(PrimitiveType::UInt64 | PrimitiveType::SInt64) => 8,
            _ => 1,
        },
        // a blob's relative offset, plus a count (array) or a type hash (variant)
        PrimitiveType::BlobString | PrimitiveType::BlobOptional => 4,
        PrimitiveType::BlobArray | PrimitiveType::BlobVariant => 8,
        PrimitiveType::ObjectReference | PrimitiveType::Guid => 16,
        PrimitiveType::None
        | PrimitiveType::Typedef
        | PrimitiveType::Struct
        | PrimitiveType::StaticArray
        | PrimitiveType::DsArray
        | PrimitiveType::DsString
        | PrimitiveType::DsOptional
        | PrimitiveType::DsVariant => 1,
    };
    (element.size as usize).max(read)
}

#[inline]
pub fn get_inner_type<T>(
    r#type: &TypeHandle<T>
) -> TypeHandle<T>
where
    T: Borrow<TypeRegistry> + Clone,
{
    r#type.inner_type()
        .map(|t| t.unwrap_typedef())
        .expect("invalid type handle: inner type is None")
}

#[inline]
pub fn get_inner_type_opt<T>(
    r#type: &TypeHandle<T>
) -> Option<TypeHandle<T>>
where
    T: Borrow<TypeRegistry> + Clone,
{
    r#type.inner_type()
        .map(|t| t.unwrap_typedef())
}

#[inline]
pub fn get_bytes(buf: &[u8], offset: usize, length: usize) -> Result<&[u8], MappingError> {
    buf.get(offset..offset + length)
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_bool(buf: &[u8], offset: usize) -> Result<bool, MappingError> {
    buf.get(offset)
        .map(|&b| b != 0)
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_u8(buf: &[u8], offset: usize) -> Result<u8, MappingError> {
    buf.get(offset)
        .copied()
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_i8(buf: &[u8], offset: usize) -> Result<i8, MappingError> {
    buf.get(offset)
        .map(|&b| b as i8)
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_u16(buf: &[u8], offset: usize) -> Result<u16, MappingError> {
    buf.get(offset..offset + 2)
        .map(|slice| u16::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_i16(buf: &[u8], offset: usize) -> Result<i16, MappingError> {
    buf.get(offset..offset + 2)
        .map(|slice| i16::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_u32(buf: &[u8], offset: usize) -> Result<u32, MappingError> {
    buf.get(offset..offset + 4)
        .map(|slice| u32::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_i32(buf: &[u8], offset: usize) -> Result<i32, MappingError> {
    buf.get(offset..offset + 4)
        .map(|slice| i32::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_u64(buf: &[u8], offset: usize) -> Result<u64, MappingError> {
    buf.get(offset..offset + 8)
        .map(|slice| u64::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_i64(buf: &[u8], offset: usize) -> Result<i64, MappingError> {
    buf.get(offset..offset + 8)
        .map(|slice| i64::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_f32(buf: &[u8], offset: usize) -> Result<f32, MappingError> {
    buf.get(offset..offset + 4)
        .map(|slice| f32::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}

#[inline]
pub fn get_f64(buf: &[u8], offset: usize) -> Result<f64, MappingError> {
    buf.get(offset..offset + 8)
        .map(|slice| f64::from_le_bytes(slice.try_into().unwrap()))
        .ok_or(MappingError::UnexpectedEndOfData)
}
