-- SPDX-License-Identifier: MIT
-- Fresh-root boundary: Windows SF rejects an empty destination without creating
-- the configured export root. Do not infer Unix file-creation semantics here.
T.expect('root.export.refused', pcall(io.export,'','raw\0\255'), false)
T.expect('root.absent', io.export_exists(''), false)
T.expect('root.read.refused', pcall(io.read_export_to_string,'.'), false)
T.expect('root.dot.refused', pcall(io.export,'.','next'), false)
T.expect('root.still.absent', io.export_exists(''), false)
T.call('root.child.export', io.export, 'child', 'bytes')
T.expect('root.child.read', io.read_export_to_string('child'), 'bytes')
T.call('root.copy.empty', io.export_copy, 'child', '')
T.call('root.rename.empty', io.export_rename, 'child', '.')
T.expect('root.file.intact', io.read_export_to_string('child'), 'bytes')
