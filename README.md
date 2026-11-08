# Lynx Rack — sequencer module (JUCE 8.0.12)

A VSTi that hosts VST3 plugins in a 16-channel rack and sequences them with step buttons and a piano roll.
Runs as a plugin in any DAW and as a standalone app. First module of the modular DAW: it keeps no DAW
baggage, and exposes a `ModuleLink` seam for the future hub app.


## Build (Windows, Visual Studio 2026)

You can build with CMAKE, however I use jucer files to indepenently build lemon scan and Lynx Rack.


## Files
    core/RackTypes.h      Note, Pattern, ChannelInfo, RackSong + versioned ValueTree (module id, schema)
    core/RackEngine.*     hosting, scheduling, mixing, snapshot hand-over, undo
    core/ChildHost.*      hosting glue on the shared scanner: plugin menu, async load (prepare -> restore state -> publish), windows
    common/               the shared AlphaAudio plugin library, same code/namespace (lemon) as Lemon Mixer Rack / FLP Organizer:
                          PluginScanner (out-of-process scan, catalog), CrashGuard (quarantine), FlDatabase (FL's plugin DB),
                          PluginDb (SQLite), HostLog
    ******* tools/LemonScan/      one-file scanner process; built with the plugin and copied beside it after every build  THIS BUILDS INDEPENDENTLY AND IS PLACED IN The shared folder, %APPDATA%\AlphaAudio\LemonMixerRack\
    core/RackProcessor.*  buses, DAW parameters, host transport, state
    core/ModuleLink.h     IModuleLink: status out / commands in (null implementation today)
    ui/                   RackEditor, RackStrip (channel rack), PianoRoll, Theme
  
## Plugin library (shared with the other modules)
- One library for all modules in `%APPDATA%\AlphaAudio\LemonMixerRack`: `plugins.xml`, `lemon_plugins.db`, `plugin_catalog.json`,
  `quarantine.txt`. Lynx Rack reads the instruments; Lemon Mixer Rack keeps reading the effects. Additive changes only:
  JSON gets an `"instruments"` key, SQLite gets a `kind` column, `catalog()` defaults to effects.
  This is a shared folder with Lemon Mixer Rack a Mixer Rack Plugin, they use the same plugin database and scan processes to populated plugins. 
  
- "Scan plugins" menu: scan VST3 folders (one LemonScan process per file, timeouts, blacklist), import FL Studio's plugin
  database (installed list, vendors, favourites incl. Generators), release quarantined plugins, open the library folder.
- A plugin that was loading / had its window open when a host crashed is quarantined automatically at next start.


## Piano roll Function
Click empty grid: draw note (drag = length). Drag note: move. Drag right edge: resize. Right click / right drag: delete.
Shift+drag: rubber band. Ctrl+drag: copy. Bottom lane: velocity. Keys: Del, Ctrl+A, Ctrl+Z, Ctrl+D, Ctrl+Q, M, arrows
(Shift = octave). Wheel: scroll; Shift horizontal; Ctrl zoom time; Alt zoom keys. Click the keyboard to preview.
