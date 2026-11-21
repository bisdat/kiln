-- SPDX-License-Identifier: MIT
-- Synthetic lexical grammar combinations. Observe SF's Windows std/camino rules.
local prefixes={'', './', 'a//', 'C:', 'c:\\', '\\', '//server/share/', '\\\\?\\C:\\', '\\\\?\\UNC\\host\\share\\', '\\\\.\\pipe\\'}
local suffixes={'', '.', '..', './.', '../.', 'a/..', 'a\\..', 'a\\.\\', 'a\\..\\b', 'a/b', '.hidden.ext', 'a//b///'}
for i, prefix in ipairs(prefixes) do
 for j, suffix in ipairs(suffixes) do
  for _, name in ipairs({'name','name_without_extension','extension','parent'}) do T.call(name..'.'..i..'.'..j, io[name], prefix..suffix) end
 end
end
for i, base in ipairs({'', 'a', 'C:', 'C:\\a', '//host/share/a', '\\\\?\\C:\\a\\..\\b', '\\\\?\\UNC\\host\\share\\a', '\\\\.\\pipe\\a'}) do
 for j, part in ipairs({'', '.', '..', '../b', '\\b', '/b', 'C:b', 'd:b', 'D:\\b', '\\\\?\\C:\\x'}) do T.call('join.'..i..'.'..j,io.join,base,part) end
end
for i, path in ipairs({'//server','//server/', '//server//', '///server/share', '\\\\?\\UNC\\', '\\\\?\\UNC\\host', '\\\\?\\', '\\\\?\\name', '\\\\.\\', 'C:/a\\./b', 'a/.//', '././a', '..\\.', 'c:..'}) do
 for _, name in ipairs({'name','name_without_extension','extension','parent'}) do T.call('edge.'..name..'.'..i,io[name],path) end
end
T.call('join.invalid',io.join,false)
T.call('join.utf8',io.join,'\255')
