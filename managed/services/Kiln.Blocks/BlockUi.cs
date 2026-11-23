// SPDX-License-Identifier: MIT
// BlockUi: what a mod's code does with its UI blocks while they run (dossier 09 §5.2): a window's state, its own data
// for tables and trees, the live game values, and commands run on the joined server. The members are spread over
// BlockUi.*.cs files by area. Kiln.Ui's static Ui cannot hold them: WindowRef is a Kiln.Blocks type.
namespace Kiln;

/// <summary>
/// UI blocks at run time: a window's state (<see cref="WindowBlock.State"/>), a mod's own data for tables and trees
/// (<see cref="Bindings"/>), the live game values windows show, and commands run on the server this game joined.
/// </summary>
public static partial class BlockUi
{
}
