# SimTransport / sim regression seeds

Seeds that once produced a real bug in `SimNetwork`/`SimTransport` or in
anything driven through them (the Wave 5 six-node acceptance suite, in
particular) get committed here as a permanent regression case, never just
noted and discarded. A failing seed found during development, sweeping, or
manual investigation belongs in this directory the same day it's found.

No fixed file format is prescribed yet - the first regression committed
here should record at minimum the seed value, the scenario that exposed it
(node count, link config, duration), and what specifically went wrong.
Finding a seed that breaks something is cheap; re-finding the same one by
accident later is not.
