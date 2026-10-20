# host/ — what every plugin does with a host

Buses and layouts (`Layout.h`, `BusChannels.h`, `ClapPorts.h`), parameters and their intake (`Parameters.h`),
transport and the control grid (`Transport.h`, `ControlGrid.h`), the shared processor
for plugins that work on a field (`FieldEffectProcessor.h`), the link node, identity and the test scaffolding (`TestPlayHead.h`, `BlockBench.h`).

- **Shared, not per plugin**: a behaviour two plugins need lives here once; a plugin configures it.
- **Nothing in `processBlock` allocates, frees or locks.** State arrives as an immutable snapshot adopted at a block
  boundary and retired on the message thread. Nothing on the audio thread touches the link bus.
- **Timing comes from the host's sample position**, anchored in whole samples — never from where a block starts, and
  never from the wall clock — so a bounce plays what realtime played.
- **Control runs on the fixed 256-sample grid**, whatever block size the host sends.
- **Test scaffolding is shared**: one fake transport (`TestPlayHead`) for every check; don't write another.
- The bus rules are in `plugins/AGENTS.md`.
