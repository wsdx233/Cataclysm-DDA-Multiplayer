# Question for the maintainer: contribution policy & ADR-0012 direction

Dear wsdx233,

I've been reviewing and independently reproducing the build/test/process-smoke path for this `Cataclysm-DDA-Multiplayer` fork (audited at commit `70da0ba360` on `multiplayer/main`), using AI-assisted tooling to help with the build orchestration and analysis.

First, I want to compliment the architecture here — the FlatBuffers protocol, the Asio transport, the single-root server owner, the session directory, and the PTY-based UI client smoke testing all fit together solidly.

Before considering any contribution, I want to make sure I'm aligned with your guidelines, since I used AI assistance for parts of this work:

### 1. Contributor policy & AI-assisted work
The inherited `CONTRIBUTING.md` prohibits AI-generated submissions, while `AGENTS.md` seems to speak to automated coding agents specifically — those read as being in tension with each other.
- Are external contributions currently welcome on this fork?
- What's your policy on AI-assisted development — does it apply strictly to core C++ game logic, or also to build automation, test scripts, and docs?

### 2. Status of ADR-0012 (monster provocation & attitude)
The docs list ADR-0012 as pending, regarding target-keyed monster provocation persistence and attitude across multiple living players.
- Has this moved since the status doc was last updated?
- What's your preferred direction for monster aggro once you go beyond `players.max == 1`?

### 3. Priority milestones
What are you currently prioritizing on this fork, and how could outside contributors best help?

Thanks for building this — opening this PR just to ask, since Issues/Discussions aren't enabled on the repo.

Best,
Daniel M. (GitHub: sangmorg1-debug)
