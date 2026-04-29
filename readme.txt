    Welcome to JNU, standing for
        J is Not Unix
    
    Disclaimer:
        Never use this in any production scenarios
        or critical software and infrastructure.

    About:
        JNU is a monolithic kernel built entirely in C and Assembly
        with the sole goal of being reliable and usable by anyone's
        standards. We currently support x86_64 and some important
        kernel infrastructure.
    
    What is the purpose of this?

    Our goal is to test the limits of vibe-coding and see
    what draws the line from human to robotic engineering.
    When we talk about vibe-coding, we don't mean to just
    send out 1 prompt to generate JNU. We use a method to
    constantly iterate. We also care about what prompts we
    are sending to the AI. Certainly not these types of prompts:


    BAD:
        Human: Generate me an OS called JNU
        AI: I will be generating an OS called JNU right now!
    
    Those are the prompts we strictly refrain from using; we use specific
    prompts or spec documents verbatim. As the author of this project, 
    I know a bit of C myself and know the technical know-how (sorta...).


    What is the audience?
            --kernel developers--

        For those who want to learn kernel development:
        read the codebase and the following coding style in 
        style.txt. We allow kernel developers to contribute freely
        in a scenario where the kernel is maturing and not overly complex.

            --vibe-coders--
        For those who want to vibe-code new features or additions, here is
        a word of caution. Pick the best models for this job, for example:

                For deep refactors or when you need deep reasoning:
                    --Anthropic--
                    Opus 4.7
                    Opus 4.6
                    --OpenAI--
                    GPT 5.5
                    GPT 5.4 (in some cases)
                    o1 / o3
                    --Google--
                    Gemini 3.5 Pro
                    Gemini 3.1 Pro (High)

                For fast and agile edits:
                    --Anthropic--
                    Haiku 4.5
                    --Google--
                    Gemini 3.5 Flash
                    Gemini 3.1 Flash
                    Gemini 3 Flash
                    --Meta--
                    Llama 4 70B
                    Llama 4 8B
                    --Open source models--
                    GLM 5/5.1
                    KIMI 2.5/2.6
                    Deepseek V4 Flash
                    Deepseek V3 Flash

                An all-rounder who is both good at coding and cost-effective:
                    --Anthropic--
                    Sonnet 4.6
                    Sonnet 4.5
                    --OpenAI--
                    GPT-4o
                    GPT-4.5
                    --Google--
                    Gemini 3.1 Pro
                    Gemini 2.5 Pro
                    --Open source models--
                    Deepseek V3
                    (More models soon, this is just my opinion.)
        
        --Operating system developers--
        For those learning how to make operating systems, I advise you
        to be careful what you read in this codebase. AIs are not accurate, 
        and they hallucinate sometimes. You'd spend more time learning accurate
        information on Linux, MikeOS, or a Rust-based OS like Redox.

    License: GPLv2.0 GNU, see LICENSE in the root file tree.    

    How to Build and Setup the Kernel:
            --Prerequisites--
        Build host: Windows 11 + WSL2 (Ubuntu 22.04+).

        In WSL, install the required toolchain:
            sudo apt install clang lld nasm make xorriso mtools

        For font generation (run once on host or inside WSL):
            pip install pillow

        QEMU runs from the Windows desktop install (e.g., installed from 
        https://qemu.weilnetz.de/). The `make run` target invokes the 
        Windows QEMU binary through a path you can override. See 
        scripts/run-qemu.ps1 for the PowerShell counterpart you can run 
        from Windows directly.

            --Bootstrap Limine (run once)--
        Clone Limine into boot/limine/:
            git clone https://github.com/limine-bootloader/limine.git \
                --branch=v8.x-binary --depth=1 boot/limine
            make -C boot/limine

            --Building the Kernel--
        From WSL, in this directory:
            make             # builds build/kernel.elf and build/kernel.iso

            --Running the Kernel--
        From Windows PowerShell:
            scripts\run-qemu.ps1

        Or from WSL (if Windows QEMU is on PATH or via $QEMU env var):
            make run

    Makefile Targets:
        Here are all the available `make` commands:

        make (or make all): Builds the kernel ELF and bootable ISO.
        make iso          : Builds the bootable ISO only.
        make font         : (Re)generates kernel/drivers/font_data.h.
        make ata-disk     : Creates (or recreates) build/disk.img. Default is 32 MiB 
                            (override with SIZE=N).
        make run          : Boots in QEMU (attaches disk if build/disk.img exists, 
                            unless NODISK is defined).
        make run-disk     : Boots in QEMU, requires the disk image to exist.
        make debug        : Boots QEMU paused (-s -S), ready for a GDB attach 
                            (attaches disk if exists).
        make debug-disk   : Boots QEMU paused, requires the disk image.
        make clean        : Removes build artifacts but keeps build/disk.img.
        make clean-disk   : Wipes build artifacts AND the ATA disk image.
        make distclean    : Deletes all build artifacts, disk image, and forces 
                            re-clone of Limine.
        make check-limine : Checks if Limine is cloned.