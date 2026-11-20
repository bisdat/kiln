// SPDX-License-Identifier: MIT
using System;
using System.Collections;
using System.Collections.Generic;

namespace Kiln.Registry;

/// <summary>A list field of a registry entry (perks, a recipe's inputs, a drop table's entries). Reading gives the list as
/// the stage sees it; Clear, Append, RemoveAt and Replace record list edits (each stacks with other mods' edits by the
/// registry's list rules: clear and replace are absolute, append and remove are relative).</summary>
[System.Diagnostics.CodeAnalysis.Experimental("KILNX031", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/registries/README.md")]
public sealed class RegistryList<T> : IReadOnlyList<T>
{
    private readonly IRegistryCells _cells;
    private readonly int _field;
    private readonly Func<IRegistryCells, int, T> _element;

    internal RegistryList(IRegistryCells cells, int field, Func<IRegistryCells, int, T> element)
    {
        _cells = cells;
        _field = field;
        _element = element;
    }

    /// <summary>How many elements the list has.</summary>
    public int Count => _cells.Count(_field);

    /// <summary>The element at an index. ArgumentOutOfRangeException outside the list.</summary>
    public T this[int index]
    {
        get
        {
            if ((uint)index >= (uint)Count)
                throw new ArgumentOutOfRangeException(nameof(index), index, $"the list has {Count} elements");
            return _element(_cells, index);
        }
    }

    /// <summary>Empties the list.</summary>
    public void Clear() => _cells.ListOp(_field, RegistryListOp.Clear, Array.Empty<object?>());

    /// <summary>Appends values (a list of values: perks, flame levels). A list of entries with fields (a recipe's inputs)
    /// appends one blank element per call of <see cref="AppendNew"/> instead.</summary>
    public void Append(params T[] values)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (values.Length == 0)
            throw new ArgumentException("nothing to append", nameof(values));
        var boxed = new object?[values.Length];
        for (int i = 0; i < values.Length; i++)
            boxed[i] = values[i];
        _cells.ListOp(_field, RegistryListOp.Append, boxed);
    }

    /// <summary>Appends a blank element to a list of entries with fields and returns it, to set its fields.</summary>
    public T AppendNew()
    {
        _cells.ListOp(_field, RegistryListOp.Append, new object?[] { null });
        return _element(_cells, Count - 1);
    }

    /// <summary>Removes the element at an index.</summary>
    public void RemoveAt(int index)
    {
        if ((uint)index >= (uint)Count)
            throw new ArgumentOutOfRangeException(nameof(index), index, $"the list has {Count} elements");
        _cells.ListOp(_field, RegistryListOp.RemoveAt, new object?[] { index });
    }

    /// <summary>Replaces the whole list with these values.</summary>
    public void Replace(IEnumerable<T> values)
    {
        ArgumentNullException.ThrowIfNull(values);
        var boxed = new List<object?>();
        foreach (var v in values)
            boxed.Add(v);
        _cells.ListOp(_field, RegistryListOp.Replace, boxed);
    }

    /// <inheritdoc/>
    public IEnumerator<T> GetEnumerator()
    {
        int n = Count;
        for (int i = 0; i < n; i++)
            yield return _element(_cells, i);
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}
