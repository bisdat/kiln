// SPDX-License-Identifier: MIT
// BlockUi.Bindings: a mod's own data for its tables, trees and values (dossier 09 §4.2, §5.2), bound as mod.<id>.
namespace Kiln;

public static partial class BlockUi
{
    /// <summary>
    /// A mod's own data for its UI blocks: define a source (a scalar, rows or a tree with typed fields) before the blocks
    /// that bind it as <c>mod.&lt;id&gt;</c>, on every side, then publish snapshots of it from any thread where there is an
    /// overlay (<c>ui.blocks.bindings</c>). Windows show the last committed
    /// publication; the mod's code is never called from a frame.
    /// <code>
    /// BlockUi.Bindings.Define("quests", new UiDataSchema(UiDataShape.Rows, Fields: [new("name", UiScalarType.String), new("done", UiScalarType.Bool)]));
    /// await BlockUi.Bindings.Publish("quests", new UiDataSnapshot.Rows([new("q1", new Dictionary&lt;string, UiScalar&gt; { ["name"] = new UiScalar.String("Find the spring"), ["done"] = new UiScalar.Bool(false) })]));
    /// </code>
    /// </summary>
    public static class Bindings
    {
        /// <summary>The capability of mods' own data.</summary>
        public const string Capability = "ui.blocks.bindings";

        /// <summary>Declares a data source of this mod (at most 64). Refused when the id is taken. Works on every side, a
        /// dedicated server included: the windows that bind <c>mod.&lt;id&gt;</c> are checked and kept there too, so define
        /// the source on every side and publish where there is an overlay.</summary>
        /// <exception cref="ArgumentException">A bad id or schema (fields: at most 16, ids <c>[a-z][a-z0-9_-]{0,63}</c>, none of the built-in key/label/parent/depth).</exception>
        /// <exception cref="InvalidOperationException">Called outside a mod's code.</exception>
        [AnyThread]
        public static UiDataRegistration Define(string id, UiDataSchema schema)
        {
            var mod = Mod();
            return UiDataRegistry.Define(mod, id, schema);
        }

        /// <summary>Publishes a snapshot of a source: copied and checked at once, committed at Kiln's next service tick
        /// (UiBindingValueChangedEvent then comes on the game's main thread). A
        /// tree with a repeated key, a missing parent or a loop, or rows with a repeated key, are refused whole (the last
        /// publication stays); a newer publication before the commit makes this one Superseded.</summary>
        /// <exception cref="ArgumentException">The snapshot does not fit the schema (shape, a missing field, a field's type).</exception>
        /// <exception cref="InvalidOperationException">Called outside a mod's code.</exception>
        /// <exception cref="KilnUnavailableException"><c>ui.blocks.bindings</c> is off.</exception>
        [ClientOnly, AnyThread, RequiresCapability(Capability)]
        public static Task<UiDataPublishResult> Publish(string id, UiDataSnapshot snapshot)
        {
            var mod = Owner();
            return UiDataRegistry.Publish(mod, id, snapshot);
        }

        private static string Owner()
        {
            if (!UiRuntime.Active) throw new KilnUnavailableException(Capability, "no overlay here: mods' data is shown only on a player's game");
            return Mod();
        }

        private static string Mod() =>
            BlocksBridge.CallerModId ?? throw new InvalidOperationException("BlockUi.Bindings is called from a mod's code: there is no mod here to own the data");
    }
}
