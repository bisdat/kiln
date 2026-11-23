---
title: Kiln modding
description: The modder's guide and API reference of Kiln, the mod framework for Enshrouded.
---
# Kiln modding

Kiln loads C# and native mods into Enshrouded. Start with [Writing Kiln mods](modding/README.md), or look up a type in
the [API reference](api/index.md): one page for each service, and each type and member says since which mod API it exists.
Press <kbd>/</kbd> to search every type, member and guide page.

<ul class="kd-start">
<li><a href="api/index.md"><span class="who">Look up</span><strong>API reference</strong><span>Every assembly, type and member a mod can use, by service.</span></a></li>
<li><a href="modding/README.md"><span class="who">Learn</span><strong>Modder's guide</strong><span>The MDK, the manifest, the lifecycle, blocks, tests and the kiln command.</span></a></li>
<li><a href="https://frostdev.io/kiln/wiki/first-mod"><span class="who">Start</span><strong>Your first mod</strong><span>From a template to a mod in a test server, on the Kiln wiki.</span></a></li>
</ul>

The obsolete namespace `Kiln.GameTypes` of Kiln.Game (the legacy component structs) is not in this reference; its
replacement is [`Kiln.Ecs`](api/Kiln.Ecs.yml). The class `Kiln.GameTypes` of Kiln.Api is.
