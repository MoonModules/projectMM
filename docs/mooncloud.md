# MoonCloud

MoonCloud is everything projectMM does with a server MoonModules runs. It is off until you switch it on, and it is one self-contained part of the software: its members are the only code that talks to a server of ours.

It is deliberately small. Each member is a separate choice with its own checkbox, because wanting one is not agreeing to the other, and each says on its own card what it exchanges before you decide. What every member sends, what never leaves your network, and how the installation id works: [privacy policy](privacy-policy.md). The controls, per member: [core system catalog](moonmodules/core/system.md#mooncloud).

## Stats

One report about this install, sent once when the firmware is installed or upgraded, and the totals from everyone else back on the same card.

### Why we collect this

projectMM is built by a small group of volunteers, so where the effort goes is the most consequential decision the project makes. Without numbers that decision is made from whoever spoke up most recently on Discord, which is a real signal but a badly skewed one: it over-weights the loud, the new, and the broken.

- **To build what is actually used.** Which effects, layouts, modifiers, drivers and services are on real devices tells us where the next improvement is worth the most. An effect on nearly every install earns polish; one almost nobody enables does not get rewritten ahead of it.
- **To know what we can stop carrying.** Every feature costs flash, memory and maintenance forever, and on an ESP32 that budget is genuinely scarce. Something no install uses is a candidate for removal, and that is very hard to justify on a hunch.
- **To test on the hardware people own.** Chip, flash, PSRAM and device model tell us which boards to keep on the bench and which variants must keep building. We would rather find a break on a board we own than have you find it.
- **To size things for real installations.** How many lights are driven, and how much memory is free, say whether a default is sensible or whether we tuned it for a device nobody runs. A layout that assumes 256 lights is the wrong default if most walls are far bigger.
- **To know whether an upgrade reached anyone.** The running version against the last reported one distinguishes an upgrade from a fresh install, which is what tells us whether a release is being picked up or a problem is stranding people on an old one.

Development is not held hostage to these numbers: something rare and excellent stays. They inform the decision rather than make it.

### What it does not do

It carries no device name, no addresses, no credentials, and nothing you typed; a unit test asserts those cannot appear in a report. There is no profile, nothing is sold, and no third party receives it. Turning it off stops all of it, and the charts go back to empty.

The totals are shown on the same card that asks: contributing earns the answer back where you already are, so you can see what saying yes gets you before you say it.

## Talk

A public message board between projectMM devices. Off until you turn it on, and a message is sent only because you typed one and pressed send.

Everything posted is public and permanent: no private message, no recipient, no delete. Your device name rides along only if you separately switch that on; otherwise messages show the first 8 characters of your installation id, which groups them without naming you.

## Sync (planned)

Device to device over the internet, for a joint show across houses. Not built yet, and not built on Stats: it shares this container and the installation id, nothing else. It will be its own opt-in, described in the privacy policy before it ships.

## Turning it off

Every member can be switched off at any time, and nothing further is sent. What was already published stays published: a message on a public board cannot be recalled, and a report already counted cannot be withdrawn. If you are unsure, leave it off; turn it on when you want to take part.
