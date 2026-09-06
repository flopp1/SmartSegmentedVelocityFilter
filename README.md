# SmartSegmentedVelocityFilter

Uses segments of each track, based off of time and then quantized to multiples/divisions of measures common in Black MIDI.
This avoids the common pitfalls of global velocity filters like SAFOR, doing unsightly stuff like cutting off phrases or funnels midway or eating into percussion or noise patterns.

Compares each segment using a global audio energy heuristic, to determine whether or not to drop or keep.

Contains various mechanisms, heuristic and blood sweat and tears of various AI models to try and target inherent problems with differentiating medium velocity audio and medium velocity arts.

Not only that, but also differentiating phrases with increasing/decreasing velocities over time with arts annoyingly made barely loud enough to slip into most filters from previous methods.

This is not a foolproof art remover, but I'd say it does a pretty damn well good job (it's even faster than SAFOR because of memory mapping and parallelism optimisations!).

Don't complain if quiet phrases or decreasing velocity phrases are cut off or whatever, those are a pain in the ass and I'm working on it.

Compile with `g++ -O3 -std=c++17 -Wall -municode -o velfilter_smart.exe velfilter_smart.cpp` or whatever the equivalent is with Clang. Don't expect MSVC to work, it's weird and quirky. 

Thankfully this README was human written, even if everything else wasn't.

## Explanations of input options (it's complicated)

Usage:
  velfilter_smart.exe input.mid output.mid [options]

Velocity (pretty intuitive):

  `--low N`              low-band cutoff (default 30) 
  
  `--high N`             high-band cutoff (default 70)
  
  `--peak N`             isolated-note rescue threshold (default 100)

Segmentation:

  `--segment-seconds N`  target segment length in wall-clock
                       seconds; boundaries snap to dyadic
                       divisions/multiples of a measure
                       (default 2.0 = one measure at the
                       120 BPM base tempo)
                       
  `--snap-divisions N`   snap grid = measure / 2^N (default 2 =
                       quarter-bar; 0 = bar lines only)
                       
^ Defaults should be fine, only change if stuff is cut off weirdly or takes a bit of time before the next segment is removed.

Masking:

  `--mask-share N`       minimum MIDI energy share (default 0.01)
  
  `--trend-threshold N`  rising-velocity tiebreaker for ambiguous
                       segments (default 12)
                       
^ I did not touch these at all in forever, it probably doesn't even do much. Relic from the earlier attempts at discerning audio and arts that was obviously not very successful, and then repurposed into god knows what. Don't touch it either.

Energy / Envelope (single exponential decay model):

  `--tau-ms N`               tau in ms for the exponential decay
                           model (default 0 = 500) - Higher makes a less steep drop off in simulated audio energy, lower a more steep drop
                           
  `--global-polyphony-cap N` approximate max concurrent audible voices;
                           caps the global energy curve to model a
                           real synth's voice stealing/limiting, so
                           a dense crash's held/decaying note count
                           can't inflate the global average past what
                           any real instrument could actually sound
                           at once (default 0, 0 disables damping) - Honestly, just leave it at 0, it doesn't do that much at all. If you still want to use it, it will basically have no effect until you reach <1000. It doesn't even cap, it just imposes a damper globally, I don't even know why, since it's like scaling everything down, which changes nothing.

Realtime audibility (intensity-relative removal scale):

  `--audibility-ratio N` own mean held-voice level vs the level of the
                       other voices sounding at the same moment;
                       segments at or above this fraction are heard
                       on a realtime (voice-limited) synth and kept
                       even where the rendered mix would bury them
                       (default 1.25, ~+2dB; 0.30 ~-10dB suits
                       sparse files)
                       
^ Fucking complicated shit that GLM came up with after 1 whole hour. Something like a low and medium velocity band filter thing, you need to raise it to like 5, 10 or 20 for fatass merges like DYHTM.
 
  `--ambient-gate N`     low-band rescue gate: the local ambient level
                       must reach this fraction of the file's
                       typical sounding level before quiet segments
                       are rescued, so art passages dominated by
                       their own quiet notes stay dropped (default
                       1.0)
                       
^ More fucking complicated shit from GLM. This one's for low velocity rescuing only but dependent on the relative energy levels of different parts, basically another attempt to filter arts and melody and noise and whatnot. Testing is around the same values as the audibility ratio on heavy merges too.
                       
^ This entire thing was absolutely fucked. Don't blame me if it's half working or half baked. I blame the fact that realtime synth and rendered audio levels and phrase audibility are completely different, especially in fatass sustain crashes and different MIDIs have audio catering to both, which is a nightmare to try and generalise for this program.

^ You MUST play around with the two values if you want good results. The optimal settings vary wildly depending on the MIDI and depend on factors the program mostly cannot quantify or easily look at from a data processing POV. Humans are just too creative in making arts that are audio and audio that are arts and audio that isn't audio and arts that aren't arts.

Other:

  `--threads N`          worker threads for the per-track scan
                       phases (default 0 = all hardware
                       threads, output is identical) - Don't change unless you want the thing to run like Master Oogway does.
                       
  `--global-bin-ms N`    masking curve resolution (default 50) - Pretty fucking coarse but I'm not interested in blowing up time complexity just for a bit more fine grainedness, which won't even matter anyway since segments are on the level of a second or two.
  
  `--verbose`            print segment decisions - If you want the program to projectile vomit information at you.
  
  `--no-prefetch`        skip the PrefetchVirtualMemory warm-up before
                       Phase 1b (Phase 1b is the first full-file
                       read; the prefetch turns a cold-cache
                       demand-page stall into one sequential read) - I hate Windows memory mapping. Don't disable prefetch or cold files will take until all hell freezes over to load from disk.
                       
  `--no-global-move`     do not consolidate globals into track 0 - I don't even know if this works, I assume it does. I don't know why you'd want this but sure.


  ### Disclaimer
  AI-written and reviewed by me. Designs are from AI but steered by me. This is not a serious project. Don't come after me with pitchforks, I was just bored.
