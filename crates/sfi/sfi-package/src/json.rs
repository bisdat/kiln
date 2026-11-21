// SPDX-License-Identifier: MIT
//! JSON values that keep document order without serde_json's `preserve_order` feature.
//!
//! ShroudForge builds with `preserve_order` (both pins' root Cargo.toml), so its package
//! documents iterate, print and serialize their object keys in the order the file wrote them.
//! sfi-package must not turn that feature on: cargo unifies features, so every crate linking
//! sfi-package (kiln-manage's inventory, for one) would get insertion-ordered maps too and
//! write its own JSON differently. [`Value`] keeps the order itself, with preserve_order's
//! map semantics: a repeated key keeps its first position and takes the last value, and
//! serde_json's `Map::remove` is [`Map::swap_remove`] there. Parsing errors are serde_json's
//! own, because serde_json's parser drives [`Value`]'s deserializer.
use indexmap::IndexMap;
use serde::{
    Deserialize, Serialize,
    de::{self, DeserializeOwned},
    ser::SerializeMap,
};
use std::fmt;

/// An object in document order.
pub type Map = IndexMap<String, Value>;

/// serde_json's value model with an ordered object. Equality ignores object order, as
/// serde_json's does with preserve_order.
#[derive(Clone, Debug, Default, PartialEq)]
pub enum Value {
    #[default]
    Null,
    Bool(bool),
    Number(serde_json::Number),
    String(String),
    Array(Vec<Value>),
    Object(Map),
}

static NULL: Value = Value::Null;

impl Value {
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.as_object()?.get(key)
    }
    pub fn get_mut(&mut self, key: &str) -> Option<&mut Value> {
        self.as_object_mut()?.get_mut(key)
    }
    pub fn as_object(&self) -> Option<&Map> {
        match self {
            Self::Object(map) => Some(map),
            _ => None,
        }
    }
    pub fn as_object_mut(&mut self) -> Option<&mut Map> {
        match self {
            Self::Object(map) => Some(map),
            _ => None,
        }
    }
    pub fn as_array(&self) -> Option<&Vec<Value>> {
        match self {
            Self::Array(items) => Some(items),
            _ => None,
        }
    }
    pub fn as_array_mut(&mut self) -> Option<&mut Vec<Value>> {
        match self {
            Self::Array(items) => Some(items),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Self::String(text) => Some(text),
            _ => None,
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Self::Bool(value) => Some(*value),
            _ => None,
        }
    }
    pub fn as_number(&self) -> Option<&serde_json::Number> {
        match self {
            Self::Number(number) => Some(number),
            _ => None,
        }
    }
    pub fn as_i64(&self) -> Option<i64> {
        self.as_number()?.as_i64()
    }
    pub fn as_u64(&self) -> Option<u64> {
        self.as_number()?.as_u64()
    }
    pub fn as_f64(&self) -> Option<f64> {
        self.as_number()?.as_f64()
    }
    pub fn is_null(&self) -> bool {
        matches!(self, Self::Null)
    }
    pub fn is_boolean(&self) -> bool {
        matches!(self, Self::Bool(_))
    }
    pub fn is_string(&self) -> bool {
        matches!(self, Self::String(_))
    }
    pub fn is_number(&self) -> bool {
        matches!(self, Self::Number(_))
    }
    pub fn is_i64(&self) -> bool {
        self.as_number().is_some_and(serde_json::Number::is_i64)
    }
    pub fn is_u64(&self) -> bool {
        self.as_number().is_some_and(serde_json::Number::is_u64)
    }
    pub fn is_f64(&self) -> bool {
        self.as_number().is_some_and(serde_json::Number::is_f64)
    }
    pub fn is_array(&self) -> bool {
        matches!(self, Self::Array(_))
    }
    pub fn is_object(&self) -> bool {
        matches!(self, Self::Object(_))
    }
    /// The same value as serde_json's type. Its objects keep this order only where
    /// serde_json's `preserve_order` is on (as in any build with sfi-core).
    pub fn to_serde(&self) -> serde_json::Value {
        match self {
            Self::Null => serde_json::Value::Null,
            Self::Bool(value) => serde_json::Value::Bool(*value),
            Self::Number(number) => serde_json::Value::Number(number.clone()),
            Self::String(text) => serde_json::Value::String(text.clone()),
            Self::Array(items) => {
                serde_json::Value::Array(items.iter().map(Self::to_serde).collect())
            }
            Self::Object(map) => serde_json::Value::Object(
                map.iter()
                    .map(|(key, value)| (key.clone(), value.to_serde()))
                    .collect(),
            ),
        }
    }
}

/// serde_json's `from_value`, with fields and entries visited in document order. [`Value`]'s
/// deserializer is serde_json's value deserializer (value/de.rs), so the messages are the ones
/// `serde_json::from_value` gives and numbers are read as stored, never through text.
pub fn from_value<T: DeserializeOwned>(value: &Value) -> Result<T, String> {
    T::deserialize(ValueDeserializer(value)).map_err(|error| error.to_string())
}

/// Whether this build's serde_json maps keep insertion order (`preserve_order`, which
/// sfi-core turns on). Without it they iterate sorted.
pub(crate) fn serde_maps_keep_order() -> bool {
    static KEEP: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *KEEP.get_or_init(|| {
        let mut map = serde_json::Map::new();
        map.insert("b".into(), serde_json::Value::Null);
        map.insert("a".into(), serde_json::Value::Null);
        map.keys().next().is_some_and(|key| key == "b")
    })
}

impl From<&serde_json::Value> for Value {
    /// Objects take the source map's iteration order.
    fn from(value: &serde_json::Value) -> Self {
        match value {
            serde_json::Value::Null => Self::Null,
            serde_json::Value::Bool(value) => Self::Bool(*value),
            serde_json::Value::Number(number) => Self::Number(number.clone()),
            serde_json::Value::String(text) => Self::String(text.clone()),
            serde_json::Value::Array(items) => Self::Array(items.iter().map(Self::from).collect()),
            serde_json::Value::Object(map) => Self::Object(
                map.iter()
                    .map(|(key, value)| (key.clone(), Self::from(value)))
                    .collect(),
            ),
        }
    }
}
impl From<serde_json::Value> for Value {
    fn from(value: serde_json::Value) -> Self {
        Self::from(&value)
    }
}
impl From<bool> for Value {
    fn from(value: bool) -> Self {
        Self::Bool(value)
    }
}
impl From<&str> for Value {
    fn from(value: &str) -> Self {
        Self::String(value.to_owned())
    }
}
impl From<String> for Value {
    fn from(value: String) -> Self {
        Self::String(value)
    }
}
impl From<i64> for Value {
    fn from(value: i64) -> Self {
        Self::Number(value.into())
    }
}
impl From<u64> for Value {
    fn from(value: u64) -> Self {
        Self::Number(value.into())
    }
}
impl From<serde_json::Number> for Value {
    fn from(value: serde_json::Number) -> Self {
        Self::Number(value)
    }
}
impl From<Vec<Value>> for Value {
    fn from(value: Vec<Value>) -> Self {
        Self::Array(value)
    }
}
impl From<Map> for Value {
    fn from(value: Map) -> Self {
        Self::Object(value)
    }
}

impl PartialEq<str> for Value {
    fn eq(&self, other: &str) -> bool {
        self.as_str() == Some(other)
    }
}
impl PartialEq<&str> for Value {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == Some(*other)
    }
}
impl PartialEq<String> for Value {
    fn eq(&self, other: &String) -> bool {
        self.as_str() == Some(other.as_str())
    }
}
impl PartialEq<bool> for Value {
    fn eq(&self, other: &bool) -> bool {
        self.as_bool() == Some(*other)
    }
}

/// A missing key, a non-object or an index past the end reads as null, as in serde_json.
impl std::ops::Index<&str> for Value {
    type Output = Value;
    fn index(&self, key: &str) -> &Value {
        self.get(key).unwrap_or(&NULL)
    }
}
impl std::ops::Index<&String> for Value {
    type Output = Value;
    fn index(&self, key: &String) -> &Value {
        &self[key.as_str()]
    }
}
impl std::ops::Index<usize> for Value {
    type Output = Value;
    fn index(&self, index: usize) -> &Value {
        self.as_array()
            .and_then(|items| items.get(index))
            .unwrap_or(&NULL)
    }
}
/// serde_json's write access: null becomes an object, a missing key is appended as null,
/// and any other value panics.
impl std::ops::IndexMut<&str> for Value {
    fn index_mut(&mut self, key: &str) -> &mut Value {
        if self.is_null() {
            *self = Self::Object(Map::new());
        }
        match self {
            Self::Object(map) => map.entry(key.to_owned()).or_insert(Self::Null),
            other => panic!("cannot access key {key:?} in JSON {}", other.kind()),
        }
    }
}
impl std::ops::IndexMut<&String> for Value {
    fn index_mut(&mut self, key: &String) -> &mut Value {
        &mut self[key.as_str()]
    }
}
impl std::ops::IndexMut<usize> for Value {
    fn index_mut(&mut self, index: usize) -> &mut Value {
        match self {
            Self::Array(items) => {
                let len = items.len();
                items.get_mut(index).unwrap_or_else(|| {
                    panic!("cannot access index {index} of JSON array of length {len}")
                })
            }
            other => panic!("cannot access index {index} of JSON {}", other.kind()),
        }
    }
}
impl Value {
    fn kind(&self) -> &'static str {
        match self {
            Self::Null => "null",
            Self::Bool(_) => "boolean",
            Self::Number(_) => "number",
            Self::String(_) => "string",
            Self::Array(_) => "array",
            Self::Object(_) => "object",
        }
    }
}

/// serde_json's compact text (`{:#}` is its pretty text), in document order.
impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = if f.alternate() {
            serde_json::to_string_pretty(self)
        } else {
            serde_json::to_string(self)
        };
        f.write_str(&text.map_err(|_| fmt::Error)?)
    }
}

impl Serialize for Value {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Null => serializer.serialize_unit(),
            Self::Bool(value) => serializer.serialize_bool(*value),
            Self::Number(number) => number.serialize(serializer),
            Self::String(text) => serializer.serialize_str(text),
            Self::Array(items) => items.serialize(serializer),
            Self::Object(map) => {
                let mut out = serializer.serialize_map(Some(map.len()))?;
                for (key, value) in map {
                    out.serialize_entry(key, value)?;
                }
                out.end()
            }
        }
    }
}

impl<'de> Deserialize<'de> for Value {
    fn deserialize<D: de::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserializer.deserialize_any(ValueVisitor)
    }
}
/// serde_json's own value visitor (value/de.rs), building an ordered object.
struct ValueVisitor;
impl<'de> de::Visitor<'de> for ValueVisitor {
    type Value = Value;
    fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("any valid JSON value")
    }
    fn visit_bool<E>(self, value: bool) -> Result<Value, E> {
        Ok(Value::Bool(value))
    }
    fn visit_i64<E>(self, value: i64) -> Result<Value, E> {
        Ok(Value::Number(value.into()))
    }
    fn visit_u64<E>(self, value: u64) -> Result<Value, E> {
        Ok(Value::Number(value.into()))
    }
    fn visit_f64<E>(self, value: f64) -> Result<Value, E> {
        Ok(serde_json::Number::from_f64(value).map_or(Value::Null, Value::Number))
    }
    fn visit_str<E>(self, value: &str) -> Result<Value, E> {
        Ok(Value::String(value.to_owned()))
    }
    fn visit_string<E>(self, value: String) -> Result<Value, E> {
        Ok(Value::String(value))
    }
    fn visit_none<E>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }
    fn visit_some<D: de::Deserializer<'de>>(self, deserializer: D) -> Result<Value, D::Error> {
        Deserialize::deserialize(deserializer)
    }
    fn visit_unit<E>(self) -> Result<Value, E> {
        Ok(Value::Null)
    }
    fn visit_seq<A: de::SeqAccess<'de>>(self, mut seq: A) -> Result<Value, A::Error> {
        let mut items = Vec::new();
        while let Some(item) = seq.next_element()? {
            items.push(item);
        }
        Ok(Value::Array(items))
    }
    fn visit_map<A: de::MapAccess<'de>>(self, mut entries: A) -> Result<Value, A::Error> {
        let mut map = Map::new();
        while let Some((key, value)) = entries.next_entry::<String, Value>()? {
            // IndexMap::insert keeps a repeated key's first position, as preserve_order does.
            map.insert(key, value);
        }
        Ok(Value::Object(map))
    }
}

// Adapted from serde_json 1.0 src/value/de.rs (MIT OR Apache-2.0, David Tolnay): the owned
// value deserializer that serde_json::from_value uses, over an ordered object. Scalars are
// handed to serde_json's own value deserializer, so their messages are its own.
struct ValueDeserializer<'a>(&'a Value);

impl ValueDeserializer<'_> {
    fn scalar(&self) -> Option<serde_json::Value> {
        match self.0 {
            Value::Array(_) | Value::Object(_) => None,
            scalar => Some(scalar.to_serde()),
        }
    }
    fn invalid_type<E: de::Error>(&self, expected: &dyn de::Expected) -> E {
        let unexpected = match self.0 {
            Value::Array(_) => de::Unexpected::Seq,
            _ => de::Unexpected::Map,
        };
        E::invalid_type(unexpected, expected)
    }
}

fn visit_array<'de, V: de::Visitor<'de>>(
    items: &[Value],
    visitor: V,
) -> Result<V::Value, serde_json::Error> {
    let mut access = SeqAccess(items.iter());
    let value = visitor.visit_seq(&mut access)?;
    if access.0.len() == 0 {
        Ok(value)
    } else {
        Err(de::Error::invalid_length(
            items.len(),
            &"fewer elements in array",
        ))
    }
}

fn visit_object<'de, V: de::Visitor<'de>>(
    map: &Map,
    visitor: V,
) -> Result<V::Value, serde_json::Error> {
    let mut access = MapAccess {
        entries: map.iter(),
        value: None,
    };
    let value = visitor.visit_map(&mut access)?;
    if access.entries.len() == 0 {
        Ok(value)
    } else {
        Err(de::Error::invalid_length(
            map.len(),
            &"fewer elements in map",
        ))
    }
}

/// Forwards to serde_json's value deserializer for a scalar; an array or object is the
/// "invalid type" serde_json gives for it.
macro_rules! scalar_only {
    ($($method:ident)*) => {$(
        fn $method<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
            match self.scalar() {
                Some(scalar) => de::Deserializer::$method(scalar, visitor),
                None => Err(self.invalid_type(&visitor)),
            }
        }
    )*};
}

impl<'de> de::Deserializer<'de> for ValueDeserializer<'_> {
    type Error = serde_json::Error;

    fn deserialize_any<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Array(items) => visit_array(items, visitor),
            Value::Object(map) => visit_object(map, visitor),
            scalar => de::Deserializer::deserialize_any(scalar.to_serde(), visitor),
        }
    }

    scalar_only! {
        deserialize_bool deserialize_i8 deserialize_i16 deserialize_i32 deserialize_i64
        deserialize_i128 deserialize_u8 deserialize_u16 deserialize_u32 deserialize_u64
        deserialize_u128 deserialize_f32 deserialize_f64 deserialize_char deserialize_str
        deserialize_string deserialize_unit deserialize_identifier
    }

    fn deserialize_option<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Null => visitor.visit_none(),
            _ => visitor.visit_some(self),
        }
    }

    fn deserialize_enum<V: de::Visitor<'de>>(
        self,
        name: &'static str,
        variants: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Object(map) => {
                let mut entries = map.iter();
                let (Some((variant, value)), None) = (entries.next(), entries.next()) else {
                    return Err(de::Error::invalid_value(
                        de::Unexpected::Map,
                        &"map with a single key",
                    ));
                };
                visitor.visit_enum(EnumAccess { variant, value })
            }
            Value::Array(_) => Err(de::Error::invalid_type(
                de::Unexpected::Seq,
                &"string or map",
            )),
            scalar => {
                de::Deserializer::deserialize_enum(scalar.to_serde(), name, variants, visitor)
            }
        }
    }

    fn deserialize_newtype_struct<V: de::Visitor<'de>>(
        self,
        _name: &'static str,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        visitor.visit_newtype_struct(self)
    }

    fn deserialize_bytes<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Array(items) => visit_array(items, visitor),
            Value::Object(_) => Err(self.invalid_type(&visitor)),
            scalar => de::Deserializer::deserialize_bytes(scalar.to_serde(), visitor),
        }
    }

    fn deserialize_byte_buf<V: de::Visitor<'de>>(
        self,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        self.deserialize_bytes(visitor)
    }

    fn deserialize_unit_struct<V: de::Visitor<'de>>(
        self,
        _name: &'static str,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        self.deserialize_unit(visitor)
    }

    fn deserialize_seq<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Array(items) => visit_array(items, visitor),
            Value::Object(_) => Err(self.invalid_type(&visitor)),
            scalar => de::Deserializer::deserialize_seq(scalar.to_serde(), visitor),
        }
    }

    fn deserialize_tuple<V: de::Visitor<'de>>(
        self,
        _len: usize,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        self.deserialize_seq(visitor)
    }

    fn deserialize_tuple_struct<V: de::Visitor<'de>>(
        self,
        _name: &'static str,
        _len: usize,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        self.deserialize_seq(visitor)
    }

    fn deserialize_map<V: de::Visitor<'de>>(self, visitor: V) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Object(map) => visit_object(map, visitor),
            Value::Array(_) => Err(self.invalid_type(&visitor)),
            scalar => de::Deserializer::deserialize_map(scalar.to_serde(), visitor),
        }
    }

    fn deserialize_struct<V: de::Visitor<'de>>(
        self,
        name: &'static str,
        fields: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Array(items) => visit_array(items, visitor),
            Value::Object(map) => visit_object(map, visitor),
            scalar => {
                de::Deserializer::deserialize_struct(scalar.to_serde(), name, fields, visitor)
            }
        }
    }

    fn deserialize_ignored_any<V: de::Visitor<'de>>(
        self,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        visitor.visit_unit()
    }
}

struct SeqAccess<'a>(std::slice::Iter<'a, Value>);
impl<'de> de::SeqAccess<'de> for SeqAccess<'_> {
    type Error = serde_json::Error;
    fn next_element_seed<T: de::DeserializeSeed<'de>>(
        &mut self,
        seed: T,
    ) -> Result<Option<T::Value>, Self::Error> {
        self.0
            .next()
            .map(|item| seed.deserialize(ValueDeserializer(item)))
            .transpose()
    }
    fn size_hint(&self) -> Option<usize> {
        match self.0.size_hint() {
            (lower, Some(upper)) if lower == upper => Some(upper),
            _ => None,
        }
    }
}

struct MapAccess<'a> {
    entries: indexmap::map::Iter<'a, String, Value>,
    value: Option<&'a Value>,
}
impl<'de> de::MapAccess<'de> for MapAccess<'_> {
    type Error = serde_json::Error;
    fn next_key_seed<K: de::DeserializeSeed<'de>>(
        &mut self,
        seed: K,
    ) -> Result<Option<K::Value>, Self::Error> {
        let Some((key, value)) = self.entries.next() else {
            return Ok(None);
        };
        self.value = Some(value);
        map_key(key, seed).map(Some)
    }
    fn next_value_seed<T: de::DeserializeSeed<'de>>(
        &mut self,
        seed: T,
    ) -> Result<T::Value, Self::Error> {
        match self.value.take() {
            Some(value) => seed.deserialize(ValueDeserializer(value)),
            None => Err(de::Error::custom("value is missing")),
        }
    }
    fn size_hint(&self) -> Option<usize> {
        match self.entries.size_hint() {
            (lower, Some(upper)) if lower == upper => Some(upper),
            _ => None,
        }
    }
}

/// A key through serde_json's own map-key deserializer (numeric keys parse from their text,
/// with its messages): the key of a one-entry serde_json object, read by `seed`.
fn map_key<'de, K: de::DeserializeSeed<'de>>(
    key: &str,
    seed: K,
) -> Result<K::Value, serde_json::Error> {
    struct KeyOf<K>(K);
    impl<'de, K: de::DeserializeSeed<'de>> de::Visitor<'de> for KeyOf<K> {
        type Value = K::Value;
        fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str("a map key")
        }
        fn visit_map<A: de::MapAccess<'de>>(self, mut entries: A) -> Result<K::Value, A::Error> {
            let key = entries
                .next_key_seed(self.0)?
                .ok_or_else(|| de::Error::custom("value is missing"))?;
            entries.next_value::<de::IgnoredAny>()?;
            Ok(key)
        }
    }
    let mut one = serde_json::Map::new();
    one.insert(key.to_owned(), serde_json::Value::Null);
    de::Deserializer::deserialize_map(serde_json::Value::Object(one), KeyOf(seed))
}

struct EnumAccess<'a> {
    variant: &'a String,
    value: &'a Value,
}
impl<'de, 'a> de::EnumAccess<'de> for EnumAccess<'a> {
    type Error = serde_json::Error;
    type Variant = VariantAccess<'a>;
    fn variant_seed<V: de::DeserializeSeed<'de>>(
        self,
        seed: V,
    ) -> Result<(V::Value, Self::Variant), Self::Error> {
        let variant = <String as de::IntoDeserializer<'de, serde_json::Error>>::into_deserializer(
            self.variant.clone(),
        );
        let value = seed.deserialize(variant)?;
        Ok((value, VariantAccess(self.value)))
    }
}

struct VariantAccess<'a>(&'a Value);
impl<'de> de::VariantAccess<'de> for VariantAccess<'_> {
    type Error = serde_json::Error;
    fn unit_variant(self) -> Result<(), Self::Error> {
        Deserialize::deserialize(ValueDeserializer(self.0))
    }
    fn newtype_variant_seed<T: de::DeserializeSeed<'de>>(
        self,
        seed: T,
    ) -> Result<T::Value, Self::Error> {
        seed.deserialize(ValueDeserializer(self.0))
    }
    fn tuple_variant<V: de::Visitor<'de>>(
        self,
        _len: usize,
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Array(items) if items.is_empty() => visitor.visit_unit(),
            Value::Array(items) => visit_array(items, visitor),
            other => Err(de::Error::invalid_type(unexpected(other), &"tuple variant")),
        }
    }
    fn struct_variant<V: de::Visitor<'de>>(
        self,
        _fields: &'static [&'static str],
        visitor: V,
    ) -> Result<V::Value, Self::Error> {
        match self.0 {
            Value::Object(map) => visit_object(map, visitor),
            other => Err(de::Error::invalid_type(
                unexpected(other),
                &"struct variant",
            )),
        }
    }
}

/// serde_json's Value::unexpected.
fn unexpected(value: &Value) -> de::Unexpected<'_> {
    match value {
        Value::Null => de::Unexpected::Unit,
        Value::Bool(value) => de::Unexpected::Bool(*value),
        Value::Number(number) => match (number.as_u64(), number.as_i64(), number.as_f64()) {
            (Some(value), _, _) => de::Unexpected::Unsigned(value),
            (None, Some(value), _) => de::Unexpected::Signed(value),
            (None, None, value) => de::Unexpected::Float(value.unwrap_or_default()),
        },
        Value::String(text) => de::Unexpected::Str(text),
        Value::Array(_) => de::Unexpected::Seq,
        Value::Object(_) => de::Unexpected::Map,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::de::DeserializeOwned;

    #[derive(Debug, PartialEq, Deserialize)]
    #[serde(rename_all = "kebab-case")]
    enum Unit {
        Patch,
        Runtime,
    }
    #[derive(Debug, PartialEq, Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Closed {
        setting: String,
        equals: serde_json::Value,
    }
    #[derive(Debug, PartialEq, Deserialize)]
    struct Pair {
        b: u32,
        a: u32,
    }

    /// The old reader: serde_json::from_value on serde_json's own value.
    fn serde_reads<T: DeserializeOwned>(text: &str) -> Result<T, String> {
        let value: serde_json::Value = serde_json::from_str(text).unwrap();
        serde_json::from_value(value).map_err(|e| e.to_string())
    }
    fn ours<T: DeserializeOwned>(text: &str) -> Result<T, String> {
        from_value(&serde_json::from_str::<Value>(text).unwrap())
    }
    fn same<T: DeserializeOwned + PartialEq + fmt::Debug>(text: &str) {
        assert_eq!(ours::<T>(text), serde_reads::<T>(text), "{text}");
    }

    /// ShroudForge checks profile capabilities, controls and conditions with
    /// serde_json::from_value (mod_profiles.rs:110, read_mod_settings.rs:83,127), so its
    /// messages are the value deserializer's, not the text parser's.
    #[test]
    fn from_value_gives_serde_json_from_value_messages() {
        for text in [
            r#"["patch","runtime"]"#,
            "[5]",
            "[null]",
            "[1.5]",
            "[true]",
            "[[]]",
            "[{}]",
            r#"[{"patch":null}]"#,
            r#"[{"patch":1}]"#,
            r#"[{"patch":null,"runtime":null}]"#,
            r#"["nope"]"#,
            r#""patch""#,
            "{}",
        ] {
            same::<Vec<Unit>>(text);
        }
        for text in ["5", "null", "[]", r#"{"a":1}"#, r#""slider""#] {
            same::<Unit>(text);
        }
        for text in [
            r#"{"equals":1,"setting":"x"}"#,
            r#"{"equals":1,"setting":"x","z":1}"#,
            r#"{"setting":"x"}"#,
            r#"["x",true]"#,
            r#"["x",true,3]"#,
            r#"["x"]"#,
            "5",
            "null",
        ] {
            same::<Closed>(text);
        }
        for text in ["[1,2]", "[1,2,3]", "[1]", r#"{"0":1}"#] {
            same::<(u8, u8)>(text);
            same::<[u8; 2]>(text);
        }
        for text in ["300", "-1", "1.0", r#""1""#, "[]", "{}"] {
            same::<u8>(text);
        }
        for text in [r#"{"1":2}"#, r#"{"x":2}"#, r#"{"01":2}"#, r#"{"1 ":2}"#] {
            same::<std::collections::BTreeMap<u8, u8>>(text);
        }
        for text in [r#""x""#, "null", "7", "[7]"] {
            same::<Option<u32>>(text);
            same::<String>(text);
            same::<Vec<u32>>(text);
        }
    }

    /// Typed reads take each number as stored; nothing goes through text again.
    #[test]
    fn from_value_keeps_every_float_bit() {
        let mut state = 0x9e37_79b9_7f4a_7c15_u64;
        for _ in 0..200_000 {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let x = f64::from_bits(state);
            let Some(number) = serde_json::Number::from_f64(x) else {
                continue;
            };
            let read: f64 = from_value(&Value::Array(vec![Value::Number(number)]))
                .map(|v: Vec<f64>| v[0])
                .unwrap();
            assert_eq!(read.to_bits(), x.to_bits(), "{x:e}");
        }
    }

    /// Fields are visited in document order, so the first invalid one is reported.
    #[test]
    fn from_value_reports_the_first_field_in_document_order() {
        assert_eq!(
            ours::<Pair>(r#"{"a":"first","b":"second"}"#).unwrap_err(),
            r#"invalid type: string "first", expected u32"#
        );
        assert_eq!(
            ours::<Pair>(r#"{"b":"first","a":"second"}"#).unwrap_err(),
            r#"invalid type: string "first", expected u32"#
        );
        let map: IndexMap<String, u8> = ours(r#"{"z":1,"a":2,"m":3}"#).unwrap();
        assert_eq!(map.keys().collect::<Vec<_>>(), ["z", "a", "m"]);
    }
}
