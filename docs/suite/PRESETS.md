# Presets

*The same in every plugin.*

## What a preset is

**The whole sound of one plugin**: every parameter — levels included — the matrix, the source slots,
the triggers, the regions, the encoder's trajectory, Reverb's room. It is the state document without what makes an instance *this* instance:

| not in a preset | because |
|---|---|
| the instance's identity — session, id, name, colour | a preset is loaded *into* an instance |
| which preset it came from | that is the session's note, not the sound's |
| the encoder's `input.mode` | it says what is plugged in, not how it sounds; a preset that changed it would break the track it is loaded on |
| how the scene is looked at, which instance is selected | never state |

The format is the state's, so it is as tolerant: read by name, a missing key at its default, an
unknown one ignored. A preset made today loads after a parameter is added
tomorrow. Another plugin's preset is refused whole.

## Where they live

- **User presets are files**: `<user data directory>/bambi/Presets/<Plugin>/<folder>/<name>.json` (the platform's application-data
  directory; `BAMBI_PRESET_DIR` overrides it), one a preset, at most one folder deep. They belong to the user, not to a session — every project
  sees them.
- **Factory presets are built by the binary** and have no file; they are read-only. Every plugin's list
  starts with `default`, the patch a fresh instance has. Reverb adds its six rooms under `rooms/`,
  each **built from the room table** `applyPreset` reads ([REVERB](../reverb/REVERB.md)) and not
  from a copy of its numbers: a room retuned there is retuned here.
- **A user preset is a snapshot**: it holds the values it was saved with and sounds as it did, whatever
  is retuned later. A room row is a starting point that sets its parameters and the room's shape ([PICKER](PICKER.md)); a preset
  stores what it set, the selection included.
- **The session remembers the name** (`preset` in the state document), so a project reopened shows it.
- `tools/clear-presets.sh` removes every user preset. There is deliberately no button that clears them.

## Folders

**A slash in the name is the folder.** Saving as `drums/tight room` files it under `drums`; renaming to
`vocals/tight room` moves it; a name with no slash sits loose under *user*. What follows a second
slash joins the name. A folder that empties is gone. Nothing else manages folders — no "new folder",
no drag and drop. Which headings are folded is the user's and is kept beside the presets.

## The header and the browser

`‹ preset · name * ›` in the header: the chevrons step through **the whole list** in list order —
factory, then user; in each the loose ones, then folder by folder — whatever is searched or folded.
`*` says the patch has moved from the preset it names; a preset whose file is gone shows as moved.

The name opens **the browser over the panel**, as a source's page is, so the scene and the matrix
stay in sight while presets are tried:

- a **search** field with the keyboard from the moment it opens, and the host has it back the moment
  the browser closes; it matches the whole path,
  ignoring case, and lists hits flat with their folder as a tag, folded or not;
- **one click loads** (see *A load is one undo step*) and the browser stays open for the next;
- ↑ ↓ walk what is listed, return loads, escape clears a search and then closes;
- **save** (a modified user preset), **save as**, **rename**, **delete**. One row takes the name for
  both save as and rename, with the folders there are as chips under it. Saving over *another* preset
  asks first.

**Delete asks**, over the whole window — it is the one thing here an undo does not bring back; return
is yes and escape is no, and the four actions are greyed while a name is being typed. The file goes
to the Trash; the sound stays as it is, unsaved.

**The shown instance.** While the window shows another instance, a load sends it the preset's whole state --
every parameter, the patch and the preset's name -- which it applies as one undo step of its own, so a slip is undone
there as a local load is; a save writes the state the window shows and names the instance's preset without a step of
history. The files are the machine's, so the window reads them and the instance is sent only the state). A preset's folder and name travel cut to 63 characters each; a longer one is refused.
Rename and delete are the file's, and an instance already holding the old name keeps showing it.

## A load is one undo step

Host parameters are the host's to undo and everything else is the document's
— and a preset is both. So a load is the one **whole** step of the history:
the parameters are read from the host into the patch first, the step replaces the patch, and the
plugin sets the host's parameters from it — before the commit, so the block that snaps to the new
patch reads the new values. Undone, it puts back the patch *and* the parameters that were there, and
the preset's name with them; redone, it comes back to what was there when it was undone.

A load snaps the engine, as a state load does. It keeps who the instance is, and the kept keys above.

## Where the code is

Once, for every plugin:

| what | where |
|---|---|
| what a typed name means, the file format, the library, list order, search, stepping, the browser's rows, "is it modified" | `core/patch/presets` |
| the whole step | `core/patch/undo`, `host/Document` |
| load, save, rename, delete; the host's parameters set from a preset | `host::PluginProcessor` |
| the header's cluster; the browser; the question; the suite's text field | `ui::Header`, `ui::PresetBrowser`, `ui::ConfirmBox`, `ui::TextField` |
| the browser in the window | `editor::PluginEditor`, `editor::ProcessorPresets` |
| the checks, run by every plugin's suite | `editor/PresetChecks.h` |

A plugin brings data only: Reverb's factory list in its `Spec`; the encoder's kept key is a line in core.

Background: decisions 0058, 0123.
