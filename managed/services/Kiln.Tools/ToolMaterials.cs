// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using System.Text;
namespace Kiln;

/// <summary>Owned material counts from the game's normal building supplies. Observation, never payment authority.
/// Since 0.2.0-dev.30.5.</summary>
public sealed record ToolMaterialStock(ItemId Item, byte TerrainMaterial, int VoxelsPerItem, ulong Backpack, ulong MagicChests);

/// <summary>An immutable, bounded snapshot. It owns every entry and retains no game view. Since 0.2.0-dev.30.5.</summary>
public sealed class ToolMaterialSnapshot
{
    internal ToolMaterialSnapshot(ulong session,ulong revision,EntityId actor,ItemId tool,ItemId? selected,byte? material,ToolMaterialStock[] entries)
    { Session=session;Revision=revision;Actor=actor;Tool=tool;SelectedItem=selected;SelectedMaterial=material;Entries=Array.AsReadOnly((ToolMaterialStock[])entries.Clone()); }
    /// <summary>The world/session this observation belongs to.</summary>
    public ulong Session { get; }
    /// <summary>The source observation's revision.</summary>
    public ulong Revision { get; }
    /// <summary>The observed player.</summary>
    public EntityId Actor { get; }
    /// <summary>The family root, never the vanilla root's alias.</summary>
    public ItemId Tool { get; }
    /// <summary>Null when no owned, in-stock pool entry is selected.</summary>
    public ItemId? SelectedItem { get; }
    /// <summary>The selected terrain cell, or null.</summary>
    public byte? SelectedMaterial { get; }
    /// <summary>At most 64 immutable entries, in pool order.</summary>
    public IReadOnlyList<ToolMaterialStock> Entries { get; }
}

/// <summary>Read-only observations of a building tool's selector. Missing sources refuse instead of inventing counts.
/// Since 0.2.0-dev.30.5.</summary>
public static class ToolMaterials
{
    /// <summary>True when the qualified selector and owned-preview adapters are available.</summary>
    [AnyThread]
    public static bool Available => Capabilities.Is("tools.material_picker");
    /// <summary>Reads an owned observation. False when offline, on the wrong side, outside a session, or when a required
    /// symbol/source is missing; <c>Capabilities.Why("tools.material_picker")</c> explains capability refusal.</summary>
    [AnyThread]
    public static bool TrySnapshot(EntityId actor,ItemId tool,out ToolMaterialSnapshot? snapshot) => ToolsNative.Read(actor,tool,out snapshot);
}

internal sealed record ToolFamilyAction(TerrainToolAction Action,uint Program);
internal sealed record ToolFamilyMaterial(uint Item,byte Cell,int Rate);
internal sealed record ToolFamilyBrush(uint Item,double Radius);
internal sealed record ToolFamilyDefinition(Guid Identity,uint Pool,string Name,string Description,ToolFamilyAction Primary,ToolFamilyAction Secondary,ToolFamilyMaterial[] Materials,ToolFamilyBrush[] Brushes);

internal static unsafe class ToolsNative
{
    [StructLayout(LayoutKind.Sequential)] private struct Action
    { internal uint Mode,Program;internal fixed byte Remove[32];internal uint Flags,Reserved; }
    [StructLayout(LayoutKind.Sequential)] private struct Material {internal uint Item,Cell,Rate,Reserved;}
    [StructLayout(LayoutKind.Sequential)] private struct Brush {internal uint Item,Shape;internal double Radius;}
    [StructLayout(LayoutKind.Sequential)] private struct Family
    {
        internal uint Size,Tool,Pool,Reserved;internal Action Primary,Secondary;internal Str Name,Description;
        internal Material* Materials;internal uint MaterialCount,Reserved2;internal Brush* Brushes;internal uint BrushCount,Reserved3;
        internal fixed byte Identity[16];
    }
    [StructLayout(LayoutKind.Sequential)] private struct Str {internal byte* Pointer;internal nuint Length;}
    [StructLayout(LayoutKind.Sequential)] internal struct Row {internal uint Item,Cell,Rate,Reserved;internal ulong Backpack,MagicChests;}
    [StructLayout(LayoutKind.Sequential)] internal struct Snapshot
    {internal uint Size,Count;internal ulong Session,Revision;internal uint Actor,Tool,SelectedItem,SelectedMaterial;internal fixed byte Rows[2048];}
    [StructLayout(LayoutKind.Sequential)] private struct Table
    {
        internal ServiceHeader Header;internal nint Status,RegisterAction,UnregisterAction;
        internal delegate* unmanaged[Cdecl]<ulong,Family*,ulong*,int> Define;
        internal delegate* unmanaged[Cdecl]<ulong,ulong,int> Remove;
        internal delegate* unmanaged[Cdecl]<uint,uint,Snapshot*,int> Snapshot;
        internal nint PresentationStatus;
    }
    private static Table* _table;
    private static Table* T => _table!=null?_table:(_table=Services.Get<Table>("kiln.tools",1));
    internal static bool Read(EntityId actor,ItemId tool,out ToolMaterialSnapshot? value)
    {
        value=null;var t=T;if(t==null||t->Snapshot==null||actor.Value==0||tool.Value==0)return false;
        Snapshot snapshot=default;snapshot.Size=(uint)sizeof(Snapshot);
        return t->Snapshot(actor.Value,tool.Value,&snapshot)==0 && Decode(in snapshot,actor,tool,out value);
    }
    internal static bool Decode(in Snapshot source,EntityId actor,ItemId tool,out ToolMaterialSnapshot? value)
    {
        value=null;
        if(source.Size<sizeof(Snapshot)||source.Count>64||source.Session==0||source.Revision==0||source.Actor!=actor.Value||source.Tool!=tool.Value)return false;
        var entries=new ToolMaterialStock[source.Count];var items=new HashSet<uint>();var cells=new HashSet<uint>();
        ItemId? selected=null;byte? selectedCell=null;
        fixed(byte* bytes=source.Rows)for(var i=0;i<source.Count;i++) {
            var row=((Row*)bytes)[i];
            if(row.Item==0||row.Cell is <1 or >127||row.Rate is <1 or >1000000||!items.Add(row.Item)||!cells.Add(row.Cell))return false;
            entries[i]=new(new(row.Item),(byte)row.Cell,(int)row.Rate,row.Backpack,row.MagicChests);
            if(row.Item==source.SelectedItem&&row.Cell==source.SelectedMaterial&&(row.Backpack!=0||row.MagicChests!=0)) {selected=new(row.Item);selectedCell=(byte)row.Cell;}
        }
        if((source.SelectedItem!=0||source.SelectedMaterial!=0)&&selected is null)return false;
        value=new(source.Session,source.Revision,actor,tool,selected,selectedCell,entries);return true;
    }
    private static Action ActionOf(ToolFamilyAction source)
    {
        if(source.Action is null||!source.Action.TerrainOnly||source.Action.Mode is not (TerrainToolMode.Add or TerrainToolMode.Remove)||source.Program==0)
            throw new ArgumentException("A tool action needs a qualified program, mode and terrain-only policy.");
        Action value=default;value.Mode=(uint)source.Action.Mode;value.Program=source.Program;value.Flags=1;
        foreach(var cell in source.Action.RemoveMaterials??throw new ArgumentException("Missing removal materials.")) {
            if(cell is <1 or >127||source.Action.Mode!=TerrainToolMode.Remove)throw new ArgumentException("Removal cells must be terrain, on a Remove action.");
            value.Remove[cell>>3]|=(byte)(1<<(cell&7));
        }
        return value;
    }
    private static uint ItemIdOf(Guid guid) {uint hash=0x811c9dc5u;foreach(var b in guid.ToByteArray())hash=unchecked((hash^b)*0x01000193u);return hash;}
    internal static int Define(ulong owner,ToolFamilyDefinition source,out ulong handle)
    {
        handle=0;var t=T;if(t==null||t->Define==null)return 3;
        if(source.Materials.Length>64||source.Brushes.Length is <1 or >64)throw new ArgumentException("A tool family is bounded to 64 entries.");
        var name=Encoding.UTF8.GetBytes(source.Name);var description=Encoding.UTF8.GetBytes(source.Description);
        var materials=source.Materials.Select(m=>new Material {Item=m.Item,Cell=m.Cell,Rate=(uint)m.Rate}).ToArray();
        var brushes=source.Brushes.Select(b=>new Brush {Item=b.Item,Radius=b.Radius}).ToArray();
        Family value=default;value.Size=(uint)sizeof(Family);value.Tool=ItemIdOf(source.Identity);value.Pool=source.Pool;
        value.Primary=ActionOf(source.Primary);value.Secondary=ActionOf(source.Secondary);
        source.Identity.TryWriteBytes(new Span<byte>(value.Identity,16));
        fixed(byte* n=name,d=description)fixed(Material* m=materials)fixed(Brush* b=brushes) {
            value.Name=new() {Pointer=n,Length=(nuint)name.Length};value.Description=new() {Pointer=d,Length=(nuint)description.Length};
            value.Materials=m;value.MaterialCount=(uint)materials.Length;value.Brushes=b;value.BrushCount=(uint)brushes.Length;
            ulong h=0;var result=t->Define(owner,&value,&h);if(result==0)handle=h;return result;
        }
    }
    internal static void Remove(ulong owner,ulong handle) {var t=T;if(t!=null&&t->Remove!=null&&handle!=0)t->Remove(owner,handle);}
}
