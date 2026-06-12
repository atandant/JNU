Special Notes
=============

.. note::

   This section contains direct notes from the author(s) of the JNU kernel.
   These notes are not generated from source and represent first-hand
   observations, design decisions, and known caveats that do not fit neatly
   into the technical reference sections above.

Welcome to JNU!
---------------
Yeah, I built this kernel from absolute scratch using Limine v8. It was purely "vibe coded", most notably using Anthropic models. I built this entirely using free daily AI credits, so any support would motivate me to push this kernel further. I have several things on the roadmap I'd like to accomplish.

.. warning::

   Hey! Heads up: JNU is an extensive work-in-progress (WIP) and should not be used in production scenarios. 
   We haven't gone through and made it actually usable yet.

Here's a possible roadmap of all the future additions 0.0.2.3 or 0.0.3 entails.
      
      1. Codebase auditing and removal of security issues and bugs.
      2. Expect more speed when booting the kernel.
      3. Expect mouse support sometime.
      4. SMP and Demand paging.
      5. Linux Translation Shim for compatibility
      6. Booting some real world POSIX programs via our Linux Compatibiity Shim.
      7. Minix FS Write support.
      8. More filesystem drivers.

I also tried to add SLUB but that failed horribly, too messy.

