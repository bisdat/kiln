// SPDX-License-Identifier: MIT
//
// Ui.Menus for SfiNoticeTests: inside namespace Kiln.* the name Kiln.Ui is this assembly's Kiln.Ui.Tests namespace,
// not the Ui class, so the tests reach Ui.Menus through here (a namespace outside Kiln, with using Kiln).
using Kiln;

namespace SfiNoticeMenuShim;

internal static class KilnEntry
{
    public static bool Visible { set => Ui.Menus.KilnEntryVisible = value; }
}
