// SPDX-License-Identifier: MIT
namespace Kiln.Host;

// Join native registry readiness with save/config binding. Either may arrive first; an old world's settings must not
// be attached to a new native token, nor a new save id to the previous token during a world switch.
internal sealed class RegistryWorldBinding
{
    private ulong _token;
    private string? _id;
    private bool _announced;
    internal void Signal(string? directory) => _announced = directory is not null;
    internal bool Bind(RegistryWorld? candidate)
    {
        if (candidate is not { Token: not 0, Id: not null } world) return false;
        if (world.Token == _token)
        {
            if (world.Id != _id) return false;
            _announced = false;
            return true;
        }
        if (!_announced) return false;
        _token = world.Token; _id = world.Id; _announced = false;
        return true;
    }
    internal bool Matches(RegistryWorld world) => world.Token == _token && world.Id == _id && _id is not null;
}
