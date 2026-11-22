// SPDX-License-Identifier: MIT
// kiln.internal.owners v1 (native/core/src/owners_internal.h): the core's own table, resolved only through the KilnApi view
// the core hands the managed host (and Kiln's own native services). The host marks the native owner it makes for a mod
// with what that mod's kiln.mod.json opts into; kiln.net's steam_open admits an owner that holds the Steam messages grant.
// Grants go with the owner when the host revokes it (the mod's unload); a reloaded mod's new owner is granted again here.
using System.Runtime.InteropServices;

namespace Kiln.Host;

internal static unsafe class OwnerGrants
{
    public const string Name = "kiln.internal.owners";
    public const uint Version1 = 1;
    public const uint SteamMessagesGrant = 1;  // KILN_OWNER_GRANT_STEAM_MESSAGES

    /// <summary>KilnOwnersInternalApi v1 (24 bytes; v2 appends owner_id at 24, which Kiln.Host does not use): field order is the table's.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<ulong, uint, int> Grant;
        public delegate* unmanaged[Cdecl]<ulong, uint*, int> Grants;
    }

    /// <summary>Admits a mod's native owner to kiln.net's Steam message leases (<c>"steamMessages": true</c>). Never
    /// throws: without the core's table (no core, another build) the mod gets no lease (SteamMessages.Open answers
    /// NotPermitted) and kiln.log says why once per load.</summary>
    public static void SteamMessages(IHostServices host, ulong owner, string modId)
    {
        string? why;
        try
        {
            var t = (Table*)host.Service(Name, Version1);
            if (t == null)
                why = "this Kiln core does not admit mods to Steam peer messages (no kiln.internal.owners table)";
            else if (t->Version < Version1 || t->Size < (uint)sizeof(Table) || t->Grant == null)  // a later version keeps v1's members
                why = $"the core's kiln.internal.owners table is v{t->Version} with {t->Size} bytes: the core and Kiln.Host are different builds";
            else
            {
                var rc = t->Grant(owner, SteamMessagesGrant);
                why = rc == 0 ? null : $"the core refused the grant ({rc})";
            }
        }
        catch (Exception e)
        {
            why = $"{e.GetType().Name}: {e.Message}";
        }
        if (why is not null)
            host.Log(LogLevel.Warn, $"mod {modId}: \"steamMessages\": true is not in force: {why}; SteamMessages.Open answers NotPermitted");
    }
}
