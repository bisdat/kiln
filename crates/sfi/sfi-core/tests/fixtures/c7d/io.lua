-- SPDX-License-Identifier: MIT
-- Synthetic Windows path vectors and package/export bytes; no game data.
T.expect('io.alias', rawequal(io, builtin.io), true)
for _, n in ipairs({'open','close','popen','write','stdin','stdout','stderr','tmpfile','input','output','lines','flush','type'}) do
 T.expect('io.sandbox.'..n, io[n], nil)
end
for i, p in ipairs({'', '.', '..', '/', '\\', 'a', 'a/', 'a/.', './a', 'a/..', 'a/b.txt', 'a\\b.tar.gz', '.hidden', '.hidden.ext', 'a.', 'a..b', 'C:', 'C:foo', 'C:\\', 'C:\\a\\b', '\\root\\leaf', '//server/share', '//server/share/a', '\\\\?\\C:\\a\\.\\b', '\\\\?\\UNC\\server\\share\\a', '\\\\.\\pipe\\name', 'a\0b', 'é/冰.txt', 'a//b///', 'a/./b/./', '\\\\?\\C:\\a/b'}) do
 for _, n in ipairs({'name','name_without_extension','extension','parent'}) do T.call(n..'.'..i, io[n], p) end
end
for i, parts in ipairs({{}, {'a','b'}, {'a',''}, {'','a'}, {'a','../b'}, {'a','/b'}, {'C:\\a','\\b'}, {'C:\\a','D:b'}, {'C:\\a','C:b'}, {'a','C:\\b'}, {'a/','b/'}, {'\\\\?\\C:\\a','b/../c'}, {'a','.', 'b'}, {'C:', 'a'}, {12, 2.5}}) do T.call('join.'..i, io.join, table.unpack(parts)) end
for _, n in ipairs({'name','name_without_extension','extension','parent','read','read_to_string','list_files','exists','is_file','is_directory','export','export_exists','export_list','export_rename','export_copy','export_delete','read_export_to_string'}) do
 T.call(n..'.bad', io[n], false)
 T.call(n..'.missing', io[n])
 T.call(n..'.utf8', io[n], '\255')
end
T.expect('package.bytes', io.read_to_string('data.txt'), 'oracle bytes\n')
local b=io.read('data.txt')
T.expect('package.buffer', typeof(b), 'Buffer')
T.expect('package.buffer.head', b:head(), 0)
T.expect('package.buffer.tail', b:tail(), 13)
T.expect('package.exists', io.exists('data.txt'), true)
T.expect('package.file', io.is_file('data.txt'), true)
T.expect('package.dir', io.is_directory('src'), true)
T.expect('package.missing', io.exists('missing'), false)
local names=io.list_files('locales'); table.sort(names)
T.expect('package.list', names, {'locales/de.json','locales/en.json'})
T.call('export.bytes', io.export, 'c7d/a/../bytes', 'a\0\255\128')
T.expect('export.read', io.read_export_to_string('c7d/bytes'), 'a\0\255\128')
T.expect('export.exists', io.export_exists('c7d/bytes'), true)
local span=buffer.wrap('xA\0\255z'); span:head(1); span:tail(4)
T.call('export.buffer', io.export, 'c7d/span', span)
T.expect('export.buffer.bytes', io.read_export_to_string('c7d/span'), 'A\0\255')
T.expect('export.buffer.head', span:head(), 1)
T.call('export.empty', io.export, 'c7d/empty', '')
T.call('export.copy', io.export_copy, 'c7d/span', 'c7d/copy')
T.call('export.rename', io.export_rename, 'c7d/copy', 'c7d/moved')
T.expect('export.list', io.export_list('c7d'), {'c7d/bytes','c7d/empty','c7d/moved','c7d/span'})
T.call('export.delete', io.export_delete, 'c7d/moved')
T.expect('export.deleted', io.export_exists('c7d/moved'), false)
T.expect('export.missing.list', io.export_list('nothing-here'), {})
