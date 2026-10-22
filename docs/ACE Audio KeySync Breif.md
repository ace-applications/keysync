**ACE AUDIO / PRODUCT BRIEF** 

# **KeySync** 

Musical key detection plugin for FL Studio 

### **Purpose** 

Build a lightweight audio plugin that identifies the musical key of audio and, in Pro, recommends useful keys to play over it. 

**FREE EDITION** 

Detect and display the analyzed audio’s key. 

**PRO EDITION** 

Display the detected key plus compatible alternative keys and practical harmonic suggestions. 

Prepared for: Software developer / audio-plugin programmer Scope: Product requirements, workflow, technical expectations, and acceptance criteria. 

Brand note: KeySync’s defining accent is orange (#FF8A24), paired with ACE Audio’s dark navy (#080C19). 

## **1. Product overview** 

**Product name:** ACE Audio KeySync 

**Product type:** VST3 audio plugin intended for use inside FL Studio and other compatible VST3 hosts, subject to testing. 

**Core job:** Analyze audio and identify its likely musical key. The Pro edition adds useful harmonic alternatives so a producer can decide what to play next. 

The plugin should be simple to understand, provide a clear result, and communicate uncertainty when the audio does not support a reliable key estimate. 

## **2. Free and Pro editions** 

|**Capability**|**Free**|**Pro**|
|---|---|---|
|Detect the musical key|Yes|Yes|
|Display detected key (e.g., A minor)|Yes|Yes|
|Show alternative keys to play|No|Yes|
|Relative major/minor suggestions|No|Yes|
|Harmonic compatibility suggestions|No|Yes|
|BPM detection|Not currently in scope|Not currently in scope; confirm separately|
|Scale detection / transpose suggestions|No|Optional future features|



**Important product boundary: Free reveals only the detected key. Alternative-key recommendations are a Pro feature. BPM detection was discussed but is not confirmed; do not add it without approval.** 

## **3. Intended user workflow** 

- **Load the plugin:** Insert KeySync on a Mixer track in FL Studio. 

- **Provide audio:** Analyze the signal received from the host. Confirm whether v1 is live-audio only or also supports loading samples directly. 

- **Run analysis:** The user clicks Analyze/Detect. Manual triggering is preferred for the initial version because it is easier to understand and test. 

- **See the result:** Display the detected key prominently, e.g., “A minor.” If confidence is low, show “Uncertain” or “Low confidence.” 

- **Pro recommendations:** Show alternative keys and short relationship labels such as “Relative major” or “Harmonic option.” 

The plugin is an analysis assistant and should pass audio through unchanged. 

## **4. Functional requirements** 

#### **4.1 Key detection engine** 

- Estimate the tonic/root and mode (major or minor) from incoming audio where possible. 

- Analyze a sensible time window; ignore very short or silent sections. 

- Support melodies, chords, loops, samples, and full mixes. Results may be less reliable on drums-only, atonal, noisy, or heavily distorted material. 

- Use a confidence measure so the interface can flag uncertain results. 

ACE AUDIO KEYSYNC  |  PROGRAMMER BRIEF 

2 

- Handle silence and insufficient audio with a message such as “Play audio to analyze.” 

#### **4.2 Pro harmonic recommendations** 

- Generate a short ranked list of alternative keys that may work with the detected key. 

- Include relative major/minor relationships and other musically explainable options. 

- Label suggestions by their relationship to the detected key; do not imply every option fits every melody or chord progression. 

- Keep recommendation logic separate from the key detector. 

#### **4.3 Plugin behavior** 

- Pass audio through unaltered; no intentional audio processing. 

- Provide states: Idle, Analyzing, Result, Low Confidence, and No Usable Audio. 

- Avoid blocking the audio thread; schedule heavier analysis safely outside real-time processing where possible. 

- Do not store user audio by default. 

## **5. Technical expectations and deliverables** 

The programmer should propose a maintainable implementation and confirm the technical stack. A typical option is a C++ audio-plugin framework such as JUCE with a VST3 build, but alternatives are acceptable if they meet the requirements. 

- **Plugin format:** VST3 for FL Studio; confirm OS and host compatibility. 

- **Audio input:** Receive host audio and analyze it without changing the signal. 

- **Architecture:** Separate key-estimation logic, recommendation logic, and UI presentation. 

- **Performance:** No audio glitches, hangs, or blocking operations during playback. 

- **Packaging:** Provide installation steps and instructions for rescanning plugins in FL Studio. 

- **Source code:** Complete source, project/build files, dependency and license information, and reproducible build instructions. 

- **Builds:** Working VST3 binary and a debug/test build if practical. 

- **Documentation:** Setup instructions, known limitations, test results, and notes for maintaining the analysis logic. 

Identify third-party libraries, licensing obligations, and platform constraints before implementation. 

## **6. Acceptance criteria** 

- The plugin loads in a supported FL Studio version without crashing. 

- Audio passes through without audible alteration or avoidable glitches. 

- For clear, known-key loops, provide documented accuracy results; agree the test set and target with the developer before acceptance. 

- Low-confidence, silence, and insufficient-input cases are handled clearly and honestly. 

- Free users see the detected key but cannot access alternative-key recommendations. 

- Pro users see the detected key and alternative-key suggestions with relationship labels. 

- The delivered project can be rebuilt from the supplied source and documented dependencies. 

## **7. Items to confirm before development** 

These decisions are intentionally left open rather than silently assumed: 

- **Audio source for v1:** Live Mixer audio only, or direct sample loading too? 

ACE AUDIO KEYSYNC  |  PROGRAMMER BRIEF 

3 

- **BPM:** Excluded from both editions for v1, or added later / made Pro-only? The agreed Free/Pro distinction is key detection versus key recommendations. 

- **Platforms:** Windows only initially, or Windows and macOS? 

- **Pro licensing:** License key, account login, or another unlock method? 

- **Recommendation depth:** Short list of compatible keys or an advanced harmonic wheel with rankings and explanations? 

- **Commercial terms:** Source-code ownership, exclusivity, support, milestones, payment, and bug-fix period should be agreed in writing. 

## **Developer summary** 

**Build ACE Audio KeySync:** a VST3 plugin for FL Studio that detects and displays the musical key of audio. Free exposes the detected key only. Pro adds ranked alternative keys and harmonic relationship labels. Keep audio unchanged, report uncertainty honestly, and deliver complete source code, reproducible build instructions, and a tested plugin. 

ACE AUDIO KEYSYNC  |  PROGRAMMER BRIEF 

4 

