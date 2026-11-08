This is an early prototype for a "vsti channel rack"

The idea here is to make a VSTI that can act as both a vsti host and a sequencer.
The idea is to give sequencing capabilities to any vst that is loaded into it.
The idea was initially to provide one channel per instance, however it has 16 channels this may change
as there is a larger development plan to create individual modular elements such as Lemonade, Lynx Rack
and others to provide all the elements of a daw as individual modules that can load into other daws.
The idea is eventually they may be able to be cross translated and loaded into multiple points to Link
various elements together as all the components overall are like a distributed daw that can also be used
a bit like say reason rack in being able to load into another daw, or fl studios vsti instance but
for each element rather than the entire daw.

This one is the begining of a vsti host which is still very basic and daws sequencers will vastly outperform
this version of the plugin. I'd like to continue to add onto the sequencer and give it more functionality. 

The basic tests for loading the vsti are working as well as the basic sequencer 

TODO fix the always on when synced to host play function, should only be on when triggered by a midi note
in the host daw or in internal mode and play is triggered.

TO DO add some "piano roll scripting capabilities to tie in python scripting for the plugin like Alpha Sampler.

TO DO reskin in alpha theme.

Overall I hit the first milestone for this project getting audio playback and VST3 loading into the plugin.

This project is a continuation of building from Lemon Studio and Lynx Studio as well as previous plugins I've put together. It is likely to continue borrowing from past plugins. Reskinning is very likely before I put the project to testing for in a few days.   

The Project can SQLITE to help with database management.


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
