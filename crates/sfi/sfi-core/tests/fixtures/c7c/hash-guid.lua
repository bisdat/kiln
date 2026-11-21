-- SPDX-License-Identifier: MIT
-- Public-contract vectors and synthetic observations; no game or archive data.
T.expect("hasher.alias", rawequal(hasher, builtin.hasher), true)
for _, name in ipairs({"fnv1a32", "crc32", "crc64"}) do
    for _, sample in ipairs({{"empty", ""}, {"ascii", "123456789"},
        {"hello", "hello"}, {"binary", "a\0\255\128"}, {"high", "\255"}}) do
        T.call(name .. "." .. sample[1], hasher[name], sample[2])
        T.expect(name .. ".integer." .. sample[1], math.type(hasher[name](sample[2])), "integer")
    end
    local b = buffer.wrap("prefixa\0\255\128suffix")
    b:head(6)
    b:tail(10)
    T.expect(name .. ".unread", hasher[name](b), hasher[name]("a\0\255\128"))
    T.expect(name .. ".head", b:head(), 6)
    T.expect(name .. ".tail", b:tail(), 10)
    b:head(10)
    T.expect(name .. ".empty_span", hasher[name](b), hasher[name](""))
    b:tail(0)
    T.expect(name .. ".reversed_span", hasher[name](b), hasher[name](""))
    b:close()
    T.call(name .. ".closed", hasher[name], b)
    T.call(name .. ".missing", hasher[name])
    T.call(name .. ".nil", hasher[name], nil)
    T.call(name .. ".number", hasher[name], 123)
    T.call(name .. ".table", hasher[name], {})
    T.expect(name .. ".extra", hasher[name]("hello", false, nil), hasher[name]("hello"))
end
T.expect("fnv.empty.vector", hasher.fnv1a32(""), 0x811c9dc5)
T.expect("fnv.hello.vector", hasher.fnv1a32("hello"), 0x4f9f2cab)
T.expect("crc32.vector", hasher.crc32("123456789"), 0xcbf43926)
T.expect("crc64.vector", hasher.crc64("123456789"), 0x6c40df5f0b497347)
T.expect("crc64.high.bit", hasher.crc64("\255") < 0, true)
T.expect("guid.global.absent", guid, nil)
T.expect("guid.class.absent", GuidHelper, nil)
T.expect("guid.game.alias", rawequal(game, builtin.game), true)
T.expect("guid.none", game.guid.NONE, "00000000-0000-0000-0000-000000000000")
T.expect("guid.none.hash", game.guid.hash(game.guid.NONE), 0x69691905)
local words = {size=0x00112233, hash0=0x44556677, hash1=0x8899aabb, hash2=0xccddeeff}
local mixed = "00112233-6677-4455-bbaa-9988ffeeddcc"
T.expect("guid.mixed.format", game.guid.from_content_hash(words), mixed)
T.expect("guid.mixed.parse", game.guid.to_content_hash(mixed), words)
T.call("guid.mixed.hash", game.guid.hash, mixed)
T.expect("guid.hash.binary", game.guid.hash(mixed), hasher.fnv1a32("\051\034\017\000\119\102\085\068\187\170\153\136\255\238\221\204"))
for i, text in ipairs({mixed, string.upper(mixed), mixed .. "ignored", mixed .. "\0tail"}) do
    T.expect("guid.parse." .. i, game.guid.to_content_hash(text), words)
    T.expect("guid.hash." .. i, game.guid.hash(text), game.guid.hash(mixed))
end
for i, text in ipairs({"", mixed:sub(1,35), "{" .. mixed .. "}", " " .. mixed,
    mixed:gsub("-", ""), "g" .. mixed:sub(2), mixed:sub(1,8) .. "_" .. mixed:sub(10),
    mixed:sub(1,13) .. "_" .. mixed:sub(15), mixed:sub(1,18) .. "_" .. mixed:sub(20),
    mixed:sub(1,23) .. "_" .. mixed:sub(25), "é" .. mixed:sub(3), "\0" .. mixed:sub(2)}) do
    T.call("guid.parse.invalid." .. i, game.guid.to_content_hash, text)
    T.call("guid.hash.invalid." .. i, game.guid.hash, text)
end
for _, name in ipairs({"to_content_hash", "hash"}) do
    T.call("guid." .. name .. ".missing", game.guid[name])
    T.call("guid." .. name .. ".nil", game.guid[name], nil)
    T.call("guid." .. name .. ".number", game.guid[name], 123)
    T.call("guid." .. name .. ".boolean", game.guid[name], false)
    T.call("guid." .. name .. ".table", game.guid[name], {})
    T.call("guid." .. name .. ".buffer", game.guid[name], buffer.wrap(mixed))
    T.call("guid." .. name .. ".utf8", game.guid[name], "\255")
    -- String conversion validates ALL input bytes, including the ignored suffix.
    T.call("guid." .. name .. ".utf8_suffix", game.guid[name], mixed .. "\255")
end
T.call("guid.from.missing", game.guid.from_content_hash)
T.call("guid.from.nil", game.guid.from_content_hash, nil)
T.call("guid.from.number", game.guid.from_content_hash, 123)
T.call("guid.from.empty", game.guid.from_content_hash, {})
T.call("guid.from.partial", game.guid.from_content_hash, {size=1, hash0=2, hash1=3})
T.call("guid.from.negative", game.guid.from_content_hash, {size=-1, hash0=0, hash1=0, hash2=0})
T.call("guid.from.overflow", game.guid.from_content_hash, {size=4294967296, hash0=0, hash1=0, hash2=0})
T.call("guid.from.fraction", game.guid.from_content_hash, {size=1.5, hash0=0, hash1=0, hash2=0})
T.expect("guid.from.cast", game.guid.from_content_hash({size="1",hash0=2.0,hash1=3,hash2=4}), "00000001-0002-0000-0300-000004000000")
T.call("guid.from.float.overflow", game.guid.from_content_hash, {size=-1.0,hash0=1e100,hash1=0,hash2=0})
T.expect("guid.from.float.negative", game.guid.from_content_hash({size=-1.0,hash0=4294967295.0,hash1=0,hash2=0}), "00000000-ffff-ffff-0000-000000000000")
local reads = {}
local proxy = setmetatable({}, {__index=function(_, key) reads[#reads+1]=key; return words[key] end})
T.expect("guid.from.metatable", game.guid.from_content_hash(proxy), mixed)
T.expect("guid.from.order", table.concat(reads, ","), "size,hash0,hash1,hash2")
for _, value in ipairs({0, 0x7fffffff, 0x80000000, 0xffffffff}) do
    local source = {size=value,hash0=value,hash1=value,hash2=value}
    local text = game.guid.from_content_hash(source)
    T.call("guid.fields." .. value, game.guid.to_content_hash, text)
    T.expect("guid.fields.roundtrip." .. value, game.guid.to_content_hash(text), source)
    for _, field in ipairs({"size","hash0","hash1","hash2"}) do
        local a = game.guid.to_content_hash(text)
        local b = game.guid.to_content_hash(text)
        T.expect("field.type." .. field .. "." .. value, math.type(a[field]), "integer")
        T.expect("field.value." .. field .. "." .. value, a[field], value)
        T.expect("field.metatable." .. field .. "." .. value, getmetatable(a), nil)
        T.expect("field.identity." .. field .. "." .. value, rawequal(a,b), false)
        a[field] = false
        T.expect("field.replace." .. field .. "." .. value, a[field], false)
        a[field] = nil
        T.expect("field.delete." .. field .. "." .. value, a[field], nil)
        rawset(a, field, "replacement")
        T.expect("field.rawset." .. field .. "." .. value, a[field], "replacement")
        T.expect("field.independent." .. field .. "." .. value, b[field], value)
    end
end
local witness = game.guid.to_content_hash("00000001-0002-0000-0300-000004000000")
T.expect("guid.fields.witness", witness, {size=1,hash0=2,hash1=3,hash2=4})
for i, text in ipairs({"+0112233-6677-4455-bbaa-9988ffeeddcc",
    "00112233-+677-4455-bbaa-9988ffeeddcc", "00112233-6677-+455-bbaa-9988ffeeddcc",
    "00112233-6677-4455-+baa-9988ffeeddcc", "00112233-6677-4455-bbaa-9988ffeedd+c"}) do
    T.call("guid.plus." .. i, game.guid.to_content_hash, text)
    T.expect("guid.plus.rejected." .. i, pcall(game.guid.to_content_hash, text), false)
end
T.expect("hasher.no.fnv.alias", hasher.fnv, nil)
T.expect("hasher.no.crc.alias", hasher.crc, nil)
local original = hasher
hasher.test = 17
T.expect("hasher.mutable.alias", builtin.hasher.test, 17)
hasher = {}
T.expect("hasher.rebind", rawequal(hasher,builtin.hasher), false)
T.expect("hasher.alias.stable", rawequal(original,builtin.hasher), true)
game.guid.NONE = false
T.expect("guid.none.mutable", game.guid.NONE, false)
