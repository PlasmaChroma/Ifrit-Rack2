# Ifrit VST3 Host Modules
## MVP Engineering Specification

**Status:** Implementation-ready  
**Audience:** Codex / implementation agent / maintainers  
**Scope:** Two VCV Rack modules sharing one VST3 hosting subsystem  
**Working module names:** `VST-FX` and `VST-Instrument`  
**Shipping names:** To be chosen before release; use format-neutral public names because “VST” is a Steinberg trademark and should not be embedded in final product names without confirming compliance with Steinberg’s usage guidelines.

---

## 1. Executive Summary

Implement two new Ifrit modules for VCV Rack:

1. **VST-FX** — hosts a stereo VST3 audio effect.
2. **VST-Instrument** — hosts a VST3 software instrument driven by Rack polyphonic pitch, gate, velocity, and pressure CV.

The modules must share a single hosting architecture. Plugin discovery, VST3 lifecycle management, block processing, parameter enumeration, parameter learning, native editor windows, persistence, and error handling must not be independently reimplemented in each module.

Both modules expose **sixteen CV-to-plugin-parameter mapping slots**. Each slot is spatially self-contained:

```text
    CV input jack
          │
  mapping/learn element
          │
 abbreviated parameter name
```

The mapping element is a new reusable Rack widget. Clicking it enters parameter-learn mode and opens the plugin’s native editor. The next eligible plugin parameter moved by the user becomes assigned to that slot. Right-clicking provides manual assignment and clearing controls.

A larger eye control near the plugin identity display opens or hides the plugin’s native editor window.

The MVP hosts **64-bit VST3 plugins only**. VST2, CLAP, Audio Units, AAX, out-of-process plugin isolation, MPE, sidechains, multi-bus audio, and MIDI-output expanders are explicitly deferred.

---

## 2. Product Intent

The modules should make external software instruments and effects behave like coherent citizens of the Rack environment rather than like miniature DAWs bolted onto the panel.

The Rack-facing abstraction is intentionally small:

- Load one plugin.
- Open its editor through the eye control.
- Bind up to sixteen exposed automation parameters directly to sixteen CV jacks.
- Save the plugin, its state, and the mappings inside the Rack patch.
- Restore the complete hosted state when the patch is reopened.

The implementation should be architected so that CLAP can later be added behind the same backend interface without changing patches or redesigning the modules.

---

## 3. MVP Goals

### 3.1 Shared goals

- Discover installed VST3 plugin classes.
- Present a searchable plugin browser.
- Load and unload one plugin instance per Rack module.
- Open and close the plugin’s native editor in a separate top-level window.
- Process plugin audio in configurable sample blocks.
- Expose sixteen assignable automation-parameter CV inputs.
- Learn a parameter by observing edits from the plugin’s own UI.
- Allow manual parameter selection.
- Save and restore:
  - Plugin identity.
  - Plugin component state.
  - Plugin controller state.
  - Parameter mappings.
  - Host configuration.
  - Editor-window position.
- Keep loading, unloading, scanning, state-file I/O, and plugin destruction off Rack’s audio thread.
- Fail safely when a plugin is unavailable or incompatible.
- Keep all plugin-format code outside the module classes.

### 3.2 VST-FX goals

- Stereo audio input and output.
- Right input normalized from left when unpatched.
- Support the plugin’s primary audio input/output buses.
- Adapt compatible mono plugins to the stereo Rack interface.
- Provide host-level bypass.
- Preserve useful pass-through behavior when no plugin is loaded or the plugin fails.

### 3.3 VST-Instrument goals

- Polyphonic `V/OCT`, `GATE`, `VELOCITY`, and `PRESSURE` inputs.
- Up to sixteen Rack voices.
- Convert pitch and gate activity into VST3 note events.
- Produce stereo audio output.
- Provide a panic action that releases all tracked notes.
- Prevent stuck notes during plugin unload, patch reset, sample-rate changes, and module destruction.

---

## 4. Explicit Non-Goals

The following are not part of the MVP:

- VST2 hosting.
- CLAP hosting.
- Audio Unit or AAX hosting.
- 32-bit plugin binaries.
- Bridging plugins built for a different CPU architecture.
- Plugin sandboxing or crash recovery through a subprocess.
- Sidechain buses.
- More than one main audio input or output bus.
- Surround, ambisonic, or arbitrary speaker layouts.
- More than two Rack audio output channels.
- MIDI file playback.
- MIDI output from hosted plugins.
- Host expanders.
- MIDI effects.
- MPE or VST3 Note Expression control.
- Continuous per-voice pitch bend.
- Microtonal tuning.
- Audio-rate or sample-accurate CV automation.
- Per-slot attenuverters, offsets, slew controls, or range remapping.
- A generic parameter editor for plugins that provide no native editor.
- Automatic restoration of every open plugin editor when a patch loads.
- Rack transport integration or external clock input.
- Preset library management beyond whatever the plugin exposes in its native UI.
- Wet/dry mixing.
- Plugin chains.
- Multiple plugins in one module.

These features may be added later without changing the fundamental module contract.

---

## 5. Platform Strategy

### 5.1 Initial implementation target

The first correctness target is **Linux x86_64**, matching the primary Ifrit development environment.

### 5.2 Architectural requirement

Platform-specific behavior must be isolated behind interfaces from the beginning:

```text
EditorWindowPlatform
PluginPathProvider
DynamicModulePlatform
NativeRunLoopAdapter
```

Windows and macOS implementations must be addable without restructuring the module, scanner, mapping system, or audio bridge.

Unsupported platforms must either:

- Compile with hosting disabled and omit the module models, or
- Compile with explicit platform stubs that report “VST3 hosting unavailable on this platform.”

Do not leave partially functional module models visible in Rack.

### 5.3 Linux editor expectation

The Linux MVP may rely on X11/XWayland-compatible native plugin views. Wayland-native embedding is not required for the first implementation. Failure to create a plugin editor must not prevent headless audio processing.

---

## 6. Dependency and Licensing Requirements

- Use the official Steinberg VST3 SDK.
- Pin the SDK to a known commit or release.
- Vendor it as a git submodule or reproducible dependency under `dep/vst3sdk`.
- Build only the hosting components needed by Ifrit.
- Disable SDK examples, tests, plugin samples, and unrelated GUI frameworks in production builds.
- Preserve all required MIT license notices.
- Do not add JUCE for the MVP.
- Do not copy code, assets, layout, branding, or internal behavior from VCV Host.
- The module UI must be visually and structurally original to Ifrit.
- Treat `VST-FX` and `VST-Instrument` as development names only until trademark-safe public names are selected.

---

## 7. High-Level Architecture

```text
VST-FX Module ─────────────┐
                           │
VST-Instrument Module ─────┼── PluginHostController
                           │       │
Shared Rack Widgets ───────┘       ├── PluginCatalog
                                   ├── PluginScanner
                                   ├── Vst3Backend
                                   ├── HostedPluginInstance
                                   ├── AudioBlockBridge
                                   ├── ParameterMappingEngine
                                   ├── EditorWindowManager
                                   ├── HostStateSerializer
                                   └── HostCommandDispatcher
```

### 7.1 Design principle

The Rack modules are adapters. They own Rack ports, lights, parameters, and module-specific translation. They must not directly implement VST3 factory loading, processor/controller setup, editor attachment, or state stream serialization.

### 7.2 Backend abstraction

Create a format-neutral interface even though only VST3 is implemented:

```cpp
class PluginHostBackend {
public:
    virtual ~PluginHostBackend() = default;

    virtual PluginLoadResult load(const PluginDescriptor& descriptor) = 0;
    virtual void unload() = 0;

    virtual bool isLoaded() const = 0;
    virtual PluginRuntimeInfo runtimeInfo() const = 0;

    virtual bool configure(double sampleRate, uint32_t maxBlockSize) = 0;
    virtual ProcessResult process(PluginProcessBlock& block) = 0;

    virtual std::vector<PluginParameterInfo> parameters() const = 0;
    virtual bool setParameterNormalized(
        uint32_t stableParameterId,
        double normalizedValue,
        int32_t sampleOffset
    ) = 0;

    virtual PluginStateBlob captureState() = 0;
    virtual bool restoreState(const PluginStateBlob& state) = 0;

    virtual bool hasNativeEditor() const = 0;
    virtual EditorOpenResult openEditor(const EditorWindowRequest& request) = 0;
    virtual void closeEditor() = 0;
    virtual bool isEditorVisible() const = 0;
};
```

The first concrete implementation is:

```cpp
class Vst3Backend final : public PluginHostBackend;
```

Do not leak Steinberg types into module headers.

---

## 8. Suggested Source Layout

Adapt names to the existing Ifrit repository conventions, but preserve this separation:

```text
dep/
  vst3sdk/

src/
  host/
    PluginHostBackend.hpp
    PluginHostController.hpp
    PluginHostController.cpp
    PluginDescriptor.hpp
    PluginCatalog.hpp
    PluginCatalog.cpp
    PluginScanner.hpp
    PluginScanner.cpp
    HostedPluginInstance.hpp
    AudioBlockBridge.hpp
    AudioBlockBridge.cpp
    ParameterMapping.hpp
    ParameterMappingEngine.hpp
    ParameterMappingEngine.cpp
    HostCommandDispatcher.hpp
    HostStateSerializer.hpp
    HostStateSerializer.cpp
    HostStatus.hpp

  host/vst3/
    Vst3Backend.hpp
    Vst3Backend.cpp
    Vst3ModuleLoader.hpp
    Vst3ModuleLoader.cpp
    Vst3ComponentHandler.hpp
    Vst3ComponentHandler.cpp
    Vst3ParameterChanges.hpp
    Vst3EventList.hpp
    Vst3StateStream.hpp
    Vst3PluginScanner.hpp
    Vst3PluginScanner.cpp
    Vst3EditorWindow.hpp
    Vst3EditorWindow.cpp

  host/platform/
    EditorWindowPlatform.hpp
    LinuxEditorWindowPlatform.cpp
    WindowsEditorWindowPlatform.cpp
    MacEditorWindowPlatform.mm
    PluginPathProvider.hpp
    PluginPathProvider.cpp

  widgets/
    PluginIdentityDisplay.hpp
    PluginIdentityDisplay.cpp
    PluginEditorEyeButton.hpp
    PluginEditorEyeButton.cpp
    PluginParameterSlotWidget.hpp
    PluginParameterSlotWidget.cpp
    PluginParameterPicker.hpp
    PluginParameterPicker.cpp
    PluginBrowserOverlay.hpp
    PluginBrowserOverlay.cpp

  modules/
    VstFx.cpp
    VstInstrument.cpp

tests/
  host/
  modules/
```

---

## 9. Shared Module UI Contract

Both modules must have the same host-control region and mapping-bank behavior.

### 9.1 Plugin identity region

The top region contains:

- Plugin name or `NO PLUGIN`.
- Vendor, category, or format subtitle when space allows.
- Clickable plugin identity display that opens the plugin browser.
- Large eye control that opens or hides the native editor.
- Bypass/power control.
- Compact status indication.

Suggested conceptual layout:

```text
┌───────────────────────────────┐
│ PLUGIN NAME              [EYE]│
│ Vendor · VST3          [BYPASS]│
│ Status / error message         │
└───────────────────────────────┘
```

Exact visual art is outside this engineering specification, but the interaction regions must remain unambiguous at Rack scale.

### 9.2 Plugin identity display behavior

Left click:

- Open the searchable plugin browser.
- If a plugin is already loaded, preserve it until a new selection succeeds.
- Selecting `Unload` explicitly unloads the current plugin.

Right click:

- Open host context actions:
  - Rescan plugins.
  - Add search directory.
  - Remove custom search directory.
  - Show incompatible plugins.
  - Unload plugin.
  - Copy diagnostic information.
  - Host tempo.
  - Processing block size.
  - Reset editor position.

### 9.3 Eye button behavior

The eye button controls only the native editor window.

States:

- **Dark/closed:** no plugin loaded or editor hidden.
- **Dim enabled:** plugin loaded and editor available.
- **Bright/open:** editor currently visible.
- **Pulsing:** editor is being created.
- **Fault state:** editor creation failed.
- **Disabled:** plugin provides no native editor.

Left click:

- If the editor is hidden, open/show it.
- If the editor is visible, hide/close it.
- If no plugin is loaded, do nothing beyond optional tooltip feedback.

The eye state must synchronize when the user closes the native editor through the operating system window controls.

### 9.4 Bypass behavior

Bypass is host-owned and must always be available.

- Bypass must not unload the plugin.
- Parameter mappings and state remain intact.
- The plugin may continue receiving process calls to avoid breaking tails or internal state.
- For VST-FX, bypass outputs the dry Rack input.
- For VST-Instrument, bypass mutes plugin audio output while continuing note tracking.
- A short crossfade of approximately 32 samples should be used when entering or leaving bypass to avoid hard discontinuities.
- If a plugin exposes an official VST3 bypass parameter, it may also be set, but host behavior must not depend on its existence.

---

## 10. Sixteen Parameter Mapping Slots

### 10.1 Layout

Each module exposes exactly sixteen mapping slots in the MVP.

Each slot contains:

1. A standard Rack CV input jack.
2. A small mapping/learn element immediately below the jack.
3. A dynamic parameter label immediately below the mapping element.

Example:

```text
       ◯
      <◉>
    CUTOFF
```

The jack and its mapping control must be visibly co-located. The user should never need to trace a row from a detached label to a distant jack.

### 10.2 New widget class

Implement a reusable class:

```cpp
class PluginParameterSlotWidget : public rack::widget::Widget;
```

Recommended supporting classes:

```cpp
class PluginParameterMapButton : public rack::widget::OpaqueWidget;
class PluginParameterNameDisplay : public rack::widget::Widget;
class PluginParameterTooltip;
```

The standard Rack `PortWidget` should remain registered directly with the parent `ModuleWidget` through `addInput()`. The composite slot widget should coordinate the mapping button, label, tooltip, and layout, but should not bypass Rack’s normal port registration.

A helper factory is recommended:

```cpp
PluginParameterSlotHandle addPluginParameterSlot(
    rack::app::ModuleWidget& owner,
    rack::math::Vec jackCenter,
    int inputId,
    int slotIndex
);
```

### 10.3 Slot states

Each slot must visually distinguish:

- `Unmapped`
- `Learning`
- `MappedIdle`
- `MappedActive`
- `MappedMissingParameter`
- `UnavailableNoPlugin`

The mapping element may use the Ifrit eye/glyph language, but state must remain readable without relying only on subtle animation.

### 10.4 Left-click behavior

For an unmapped slot:

1. Enter learn mode for that slot.
2. Cancel learn mode on any other slot.
3. Open the plugin editor if available and hidden.
4. Change the label to `MOVE CONTROL`.
5. Capture the next eligible parameter edit from the plugin controller.
6. Store the parameter’s stable VST3 `ParamID`.
7. Replace the temporary label with the parameter short title or abbreviated title.

For a mapped slot:

- Left click begins remapping through the same learn process.
- Clicking the slot again while it is already learning cancels learn mode.

Only one learn operation may be active per hosted plugin instance.

### 10.5 Right-click behavior

The context menu must include:

- `Learn from plugin UI`
- `Choose parameter…`
- `Clear mapping`
- `Show plugin editor`
- Mapping information:
  - Full parameter name.
  - Unit.
  - Stable parameter ID.
  - Continuous/discrete status.
  - Current normalized value.

`Choose parameter…` opens a shared searchable parameter picker. Do not create a single enormous native Rack menu containing hundreds or thousands of parameter entries.

### 10.6 Learn capture rules

The VST3 component handler receives plugin UI edit callbacks. Learn mode captures the next parameter that:

- Exists in the current parameter catalog.
- Is not read-only.
- Is marked automatable when that information is available.
- Is not an internal host-only pseudo-parameter.
- Is not already being ignored because it is part of plugin initialization chatter.

A valid `beginEdit` / `performEdit` gesture should be preferred. If a plugin does not emit a clean gesture, a `performEdit` callback may still be accepted after learn mode starts.

Initialization callbacks occurring immediately after plugin load must not accidentally satisfy a learn request.

### 10.7 Mapping persistence

Store, per slot:

```cpp
struct ParameterMapping {
    bool assigned = false;
    uint32_t parameterId = 0;

    std::string cachedTitle;
    std::string cachedShortTitle;
    std::string cachedUnit;

    int32_t cachedStepCount = 0;
    uint32_t cachedFlags = 0;
};
```

The stable parameter ID is authoritative. Cached labels are for display when the plugin is missing or the parameter cannot be resolved.

### 10.8 CV conversion

For an assigned slot with a connected cable:

```text
Rack CV:      0 V ───────────── 10 V
Normalized:   0.0 ───────────── 1.0
```

Conversion:

```cpp
double normalized = rack::math::clamp(voltage / 10.0, 0.0, 1.0);
```

For discrete parameters:

- Quantize using the parameter’s `stepCount`.
- Ensure the highest CV maps to the highest legal discrete value.
- Do not assume all parameters are continuous.

For an unpatched mapped slot:

- Do not write automation values.
- Permit the plugin UI and plugin state to control the parameter normally.

When a cable is disconnected:

- Stop host automation.
- Leave the parameter at its most recent value.
- Do not jump back to the value that existed before the cable was connected.

### 10.9 MVP automation rate

The MVP sends at most one automation point per mapped parameter per processing block.

- Sample the most recent CV value available when a block is submitted.
- Place the parameter point at sample offset `0`.
- Do not enqueue a new point when the normalized value has not materially changed.
- Use a small epsilon for continuous parameters.
- Always send a change when a discrete step changes.

Display a tooltip warning that very fast modulation may be expensive or unstable in third-party plugins.

Sample-accurate automation and within-block CV capture are deferred.

---

## 11. Parameter Catalog

After a plugin loads, enumerate its parameters through the edit controller and produce immutable host-facing records:

```cpp
struct PluginParameterInfo {
    uint32_t id;
    std::string title;
    std::string shortTitle;
    std::string units;

    int32_t stepCount;
    double defaultNormalized;
    uint32_t flags;
    int32_t unitId;

    bool canAutomate;
    bool isReadOnly;
    bool isBypass;
    bool isProgramChange;
};
```

Requirements:

- Preserve the VST3 `ParamID`; never persist list indices.
- Maintain an `id -> index` lookup.
- Refresh the catalog when the plugin reports parameter-title or parameter-structure changes.
- Resolve current normalized values through the controller.
- Keep the audio thread’s parameter lookup allocation-free.
- Allow the UI to use an immutable snapshot without locking the plugin instance.

If a stored mapping ID no longer exists after loading:

- Preserve the mapping record.
- Mark the slot `MappedMissingParameter`.
- Do not map it to another parameter by index.
- Permit the user to remap or clear it.

---

## 12. Plugin Discovery and Catalog

### 12.1 Catalog entry

A catalog entry represents one loadable plugin class, not merely one `.vst3` bundle:

```cpp
struct PluginDescriptor {
    std::string format;          // "VST3"
    std::string modulePath;
    std::string classId;
    std::string name;
    std::string vendor;
    std::string category;
    std::string subcategories;
    std::string version;

    bool appearsInstrument;
    bool appearsEffect;
    bool hasAudioInput;
    bool hasAudioOutput;
    bool hasEventInput;
};
```

A single bundle may produce multiple descriptors.

### 12.2 Search locations

- Scan platform-standard VST3 locations.
- Support user-added directories.
- Search recursively where appropriate.
- Canonicalize paths and deduplicate entries.
- Persist custom directories and catalog cache in Ifrit plugin-level settings, not in individual patches.

### 12.3 Scanning behavior

The scan must not run on Rack’s engine thread.

MVP scanning may occur in-process on a dedicated worker thread. This limitation must be documented: a badly behaved plugin may crash Rack during scanning or loading. Out-of-process scanning is deferred.

Prefer metadata inspection that does not instantiate the processor when possible. If a module must be loaded to enumerate plugin classes, release it fully after inspection.

### 12.4 Browser filtering

VST-FX default filter:

- Show plugins that expose a compatible primary audio input and audio output.
- Prefer effect categories.
- Hide obvious instrument-only classes unless `Show incompatible plugins` is enabled.

VST-Instrument default filter:

- Show plugins with an event input and audio output.
- Prefer instrument categories.
- Hide obvious effect-only classes unless `Show incompatible plugins` is enabled.

The final compatibility check occurs during loading, not solely from cached metadata.

### 12.5 Plugin browser

Implement a custom searchable overlay or modal-style Rack widget with:

- Search field.
- Vendor grouping.
- Category grouping.
- Favorites optional only if trivial; otherwise defer.
- Plugin name.
- Vendor.
- `VST3` badge.
- Compatibility indicator.
- Rescan action.
- Custom directory management.
- `Unload current plugin`.

Selecting a plugin starts asynchronous load. The currently loaded plugin remains active until the replacement has initialized successfully.

---

## 13. VST3 Plugin Lifecycle

### 13.1 Required sequence

The VST3 backend must correctly handle:

1. Load the plugin module.
2. Obtain the class factory.
3. Instantiate the selected component class.
4. Initialize the component with the host application context.
5. Obtain or instantiate the associated edit controller.
6. Initialize the controller.
7. Establish processor/controller connection points when supported.
8. Enumerate buses.
9. Select and activate supported main buses.
10. Negotiate speaker arrangement.
11. Configure process setup:
    - Realtime mode.
    - Current Rack sample rate.
    - Selected maximum block size.
    - Preferred sample size.
12. Activate the component.
13. Enter processing state.
14. Enumerate parameters.
15. Restore state when loading from a patch.
16. Create the editor only when requested.

Shutdown must reverse the relevant sequence:

1. Stop processing.
2. Deactivate component.
3. Close editor.
4. Disconnect connection points.
5. Terminate controller.
6. Terminate component.
7. Release plugin objects.
8. Unload module only when no instances use it.

### 13.2 Sample format

- Prefer 32-bit VST3 processing because Rack audio values are floats.
- If the plugin rejects 32-bit but supports 64-bit, use conversion buffers and process in 64-bit.
- Reject plugins that support neither.
- Conversion allocation must occur during configuration, not per block.

### 13.3 Bus support

MVP supports:

- One primary audio input bus for VST-FX.
- One primary audio output bus for both modules.
- One primary event input bus for VST-Instrument.
- No auxiliary, sidechain, or additional buses.

Attempt stereo arrangements first.

Compatible fallbacks:

- **Mono effect:** feed the left-normalized signal to mono input; duplicate mono output to Rack L/R.
- **Mono instrument:** duplicate mono output to Rack L/R.
- **Stereo effect/instrument:** map channels directly.
- Reject unsupported channel layouts with a clear error.

### 13.4 Plugin replacement

Loading a replacement plugin must be transactional:

- Build and configure the candidate instance off the audio thread.
- Do not destroy the active instance during candidate initialization.
- On success, swap instances at a block boundary.
- Fade between old and new output over a short transition when practical.
- Retire and destroy the old instance on the loader/control thread.
- On failure, keep the old plugin active and display the error.

---

## 14. Audio Block Bridge

Rack invokes module processing one sample at a time. VST3 processors consume blocks.

Implement:

```cpp
class AudioBlockBridge {
public:
    void configure(uint32_t blockSize, uint32_t inputChannels, uint32_t outputChannels);
    void reset();

    // Called once per Rack sample.
    void pushInputSample(float left, float right);
    StereoSample popOutputSample();

    bool blockReady() const;
    PluginProcessBlock makeProcessBlock();
    void commitProcessedBlock();
};
```

### 14.1 Block sizes

Supported values:

```text
16, 32, 64, 128, 256, 512, 1024, 2048, 4096 samples
```

Default:

```text
128 samples
```

Block size is selected from the module context menu and persisted per module.

Changing block size:

- Must occur outside the active process call.
- Requires safe processor reconfiguration.
- Clears block buffers.
- May cause a brief muted transition.
- Must not leak notes in VST-Instrument.

### 14.2 Audio scaling

Use a Rack-native nominal scale:

```text
Rack ±5 V nominal audio  <->  plugin ±1.0 nominal audio
```

Input:

```cpp
pluginSample = rack::math::clamp(rackVoltage / 5.f, -2.f, 2.f);
```

Output:

```cpp
rackVoltage = rack::math::clamp(pluginSample * 5.f, -10.f, 10.f);
```

This preserves normal Rack oscillator levels while allowing headroom up to Rack’s ±10 V full-scale convention.

Do not silently normalize or auto-gain plugin output.

### 14.3 Latency

Base host latency is approximately one selected processing block.

Additionally:

- Query plugin-reported latency when supported.
- Expose total reported latency in the module tooltip/context menu.
- No automatic Rack-wide latency compensation is required.
- Reset the bridge when plugin latency or block configuration changes.

### 14.4 Realtime process requirements

Inside `Module::process()` and the VST3 `process()` call path:

- No filesystem access.
- No plugin scanning.
- No module loading/unloading.
- No editor-window operations.
- No state serialization.
- No blocking mutex acquisition.
- No unbounded waiting.
- No heap allocation after configuration.
- No logging on every sample or every block.
- No plugin destruction.

---

## 15. VST-FX Module Specification

### 15.1 Rack ports

Inputs:

```cpp
enum InputIds {
    AUDIO_LEFT_INPUT,
    AUDIO_RIGHT_INPUT,
    PARAM_CV_INPUT_1,
    // ...
    PARAM_CV_INPUT_16,
    NUM_INPUTS
};
```

Outputs:

```cpp
enum OutputIds {
    AUDIO_LEFT_OUTPUT,
    AUDIO_RIGHT_OUTPUT,
    NUM_OUTPUTS
};
```

Parameters:

```cpp
enum ParamIds {
    BYPASS_PARAM,
    NUM_PARAMS
};
```

Lights may include:

- Plugin loaded/status.
- Bypass.
- Editor visible.
- Slot-state lights only if the custom widgets require engine-backed lights.

The eye and plugin browser controls do not need to be Rack `Param` objects unless there is a clear automation reason.

### 15.2 Input normalization

- If the left input is connected and right is disconnected, feed left to both plugin input channels.
- If both are connected, use independent channels.
- If only right is connected, use silence for left and right for right.
- If neither is connected, feed silence.

Polyphonic audio inputs are outside the MVP. Read channel zero only. Document this in tooltips.

### 15.3 No-plugin behavior

When no plugin is loaded:

- Pass audio through from input to output.
- Apply the same right-normalization rule.
- Do not add block latency.
- Mapping controls show unavailable/no-plugin state.

### 15.4 Failed-plugin behavior

If processing fails or the plugin becomes invalid:

- Stop submitting new process calls.
- Switch to dry pass-through using a short crossfade.
- Preserve plugin identity and error diagnostics.
- Allow explicit retry, reload, or unload.
- Do not repeatedly retry from the audio thread.

### 15.5 Silence flags

Where practical, populate VST3 silence flags based on the input block. Treat this as desirable but not a blocker for the first audible milestone.

---

## 16. VST-Instrument Module Specification

### 16.1 Rack ports

Inputs:

```cpp
enum InputIds {
    VOCT_INPUT,
    GATE_INPUT,
    VELOCITY_INPUT,
    PRESSURE_INPUT,

    PARAM_CV_INPUT_1,
    // ...
    PARAM_CV_INPUT_16,

    NUM_INPUTS
};
```

Outputs:

```cpp
enum OutputIds {
    AUDIO_LEFT_OUTPUT,
    AUDIO_RIGHT_OUTPUT,
    NUM_OUTPUTS
};
```

Parameters:

```cpp
enum ParamIds {
    BYPASS_PARAM,
    PANIC_PARAM,
    NUM_PARAMS
};
```

### 16.2 Voice count

Determine active Rack voice lanes from:

```cpp
voiceCount = clamp(max(VOCT channels, GATE channels), 1, 16);
```

Velocity and pressure inputs do not increase voice count. Their channel zero values broadcast naturally when they are monophonic.

If gate is unpatched, generate no notes.

If pitch is unpatched, use `0 V`, corresponding to middle C.

### 16.3 Pitch conversion

MVP pitch mode is chromatic MIDI-style note quantization:

```cpp
int note = std::lround(voctVoltage * 12.f) + 60;
note = rack::math::clamp(note, 0, 127);
```

Conventions:

- `0 V` = MIDI note 60 / middle C.
- `1/12 V` = one semitone.
- Pitch is rounded to the nearest semitone.

### 16.4 Gate detection

Track one Schmitt trigger per Rack voice.

Recommended thresholds:

- Rising threshold: approximately `1.0 V`.
- Falling threshold: approximately `0.1 V`.

On rising edge:

- Read quantized pitch.
- Allocate a unique positive VST3 `noteId`.
- Send `NoteOnEvent`.
- Store active note and noteId for the voice.

On falling edge:

- Send `NoteOffEvent` for the stored active note/noteId.
- Clear the active voice record.

### 16.5 Pitch changes while gate remains high

If the quantized note changes while a voice gate remains high:

1. Send note-off for the old note.
2. Send note-on for the new note at the same block/sample offset.
3. Allocate a new noteId.
4. Update the voice record.

This is a chromatic retrigger, not portamento.

### 16.6 Velocity

If velocity is connected:

```cpp
velocity = clamp(voltage / 10.f, 0.f, 1.f);
```

If velocity is unpatched:

```text
Default velocity = 100 / 127 ≈ 0.7874
```

Velocity is sampled when note-on is generated. Continuous velocity changes after note-on are ignored in the MVP.

### 16.7 Pressure

If pressure is connected:

```cpp
pressure = clamp(voltage / 10.f, 0.f, 1.f);
```

For each active voice:

- Sample pressure once per processing block.
- Send a poly-pressure event when it changes by more than a small epsilon.
- Associate the event with the current note/noteId where supported.
- Do not emit pressure events for inactive voices.

If the plugin or event bus does not accept pressure events, ignore them without failing note playback.

### 16.8 Event timing

The module collects note and pressure events while filling the current audio block.

Each event stores a sample offset within that block:

```cpp
struct PendingNoteEvent {
    EventType type;
    int32_t sampleOffset;
    int16_t note;
    int32_t noteId;
    float velocityOrPressure;
    int16_t channel;
};
```

Unlike parameter CV automation, note and gate events must retain their true within-block sample offsets to avoid timing jitter.

### 16.9 Event channel

Use event bus `0` and channel `0` for all voices in the MVP. Voice identity is carried through unique note IDs and note numbers.

MPE and per-channel voice allocation are deferred.

### 16.10 Panic

Panic is triggered by:

- Panel panic button.
- Context menu action.
- Plugin unload.
- Plugin replacement.
- Module reset.
- Sample-rate change.
- Block-size change.
- Module destruction.
- Patch load before replacing existing state.

Panic behavior:

1. Queue note-off events for every tracked active note when the processor is still valid.
2. Clear all voice records.
3. Clear pending event queues.
4. Reset all gate Schmitt triggers.
5. If the plugin exposes an all-notes-off mapping through supported controller parameters, it may also be invoked, but internal note tracking is authoritative.

### 16.11 No-plugin and bypass behavior

- No plugin: stereo silence.
- Failed plugin: stereo silence.
- Bypass: stereo silence, but continue consuming gate transitions and maintaining note state.
- On leaving bypass, do not retrigger currently held gates automatically. New notes begin on the next gate edge unless a future “resume held notes” option is implemented.

---

## 17. Process Context and Host Tempo

For every block, provide a valid VST3 process context containing at least:

- Sample rate.
- Project sample position.
- Continuous sample position.
- Playing state.
- Tempo.
- Time signature.

MVP defaults:

```text
Playing:       true
Tempo:         120 BPM
Time signature: 4/4
```

Host tempo is configurable from the module context menu and persisted per module.

Allowed MVP tempo range:

```text
20–300 BPM
```

A Rack clock input, reset input, start/stop transport, and tempo-following logic are deferred.

---

## 18. Native Plugin Editor

### 18.1 Window model

Open the plugin’s native view in a separate top-level operating-system window.

Do not attempt to draw the foreign editor inside the Rack panel.

### 18.2 Requirements

- Create and manipulate editor views only from the appropriate UI/main thread.
- Support plugin-requested resizing.
- Support user window resizing when the plugin declares it.
- Preserve the last known window position and size.
- Recenter the editor if the stored position is outside all current displays.
- Close the editor before unloading the plugin.
- Update the eye widget when the window is closed externally.
- Editor failure must not disable headless plugin audio.
- Opening the editor while parameter learn is active must not clear learn state.

### 18.3 Keyboard and focus

Best-effort behavior:

- Preserve normal text and keyboard input inside the plugin editor.
- Avoid stealing Rack-global keyboard input after the editor closes.
- Do not promise that Rack’s computer-keyboard MIDI system remains active while every third-party editor is focused.

### 18.4 Patch restoration

Persist editor geometry, but default to editor hidden after patch load.

Do not automatically reopen plugin editors in the MVP.

---

## 19. Threading and Command Model

### 19.1 Threads

Expected execution domains:

1. **Rack engine thread**
   - Per-sample module process.
   - Block submission to the active plugin.
   - Parameter automation queues.
   - Instrument event queues.

2. **Rack UI/main thread**
   - Rack widgets.
   - Plugin browser.
   - Eye button.
   - Native editor window lifecycle when required by platform/plugin.
   - Learn-mode UI state.

3. **Host loader/control worker**
   - Plugin scanning.
   - Plugin module loading.
   - Candidate instance construction.
   - State file I/O.
   - Plugin retirement and destruction.
   - Potential state snapshot coordination.

### 19.2 Communication

Use bounded lock-free queues, atomics, and immutable snapshots.

Suggested commands:

```cpp
enum class HostCommandType {
    LoadPlugin,
    UnloadPlugin,
    OpenEditor,
    CloseEditor,
    SetBlockSize,
    SetHostTempo,
    BeginLearn,
    CancelLearn,
    ClearMapping,
    RestoreState,
    CaptureState,
    RetryFailedPlugin
};
```

The audio thread must consume only commands specifically designed to be realtime-safe.

### 19.3 Instance swapping

Do not delete a plugin instance on the audio thread.

Use an ownership strategy such as:

- Worker builds candidate `HostedPluginInstance`.
- Audio thread atomically adopts a prepared raw/runtime pointer at a block boundary.
- Old pointer is pushed to a retirement queue.
- Worker destroys the retired instance after the audio thread can no longer access it.

Avoid `shared_ptr` destruction on the audio thread if it might invoke plugin destructors.

### 19.4 View-state snapshots

Widgets read immutable or atomic snapshots:

```cpp
struct HostViewState {
    HostStatus status;
    bool pluginLoaded;
    bool editorAvailable;
    bool editorVisible;
    bool bypassed;

    std::string pluginName;
    std::string vendor;
    std::string statusText;

    std::array<ParameterSlotViewState, 16> slots;
};
```

Drawing and mouse events must never call VST3 processor/controller methods directly.

---

## 20. Persistence

### 20.1 JSON metadata

Store small metadata through the module’s normal JSON serialization:

```json
{
  "schemaVersion": 1,
  "plugin": {
    "format": "VST3",
    "modulePath": "...",
    "classId": "...",
    "name": "...",
    "vendor": "..."
  },
  "host": {
    "blockSize": 128,
    "tempo": 120.0,
    "bypassed": false
  },
  "editor": {
    "x": 0,
    "y": 0,
    "width": 0,
    "height": 0
  },
  "mappings": [
    {
      "slot": 0,
      "assigned": true,
      "parameterId": 1234,
      "title": "Cutoff",
      "shortTitle": "Cutoff",
      "unit": "Hz",
      "stepCount": 0,
      "flags": 0
    }
  ]
}
```

Do not serialize opaque plugin state as base64 JSON.

### 20.2 Patch storage

Use Rack per-module patch storage for opaque state:

```text
<module patch storage>/
  vst3-component-state.bin
  vst3-controller-state.bin
  host-state-manifest.json
```

State capture sequence:

1. Capture processor/component state.
2. Capture controller state.
3. Write temporary files.
4. Atomically replace existing state files.
5. Update manifest only after successful writes.

State restore sequence:

1. Load and initialize the plugin.
2. Apply component state to the component.
3. Apply component state to the controller.
4. Apply controller state to the controller.
5. Re-enumerate parameters.
6. Resolve mappings by `ParamID`.
7. Begin processing.
8. Keep editor hidden.

### 20.3 Save coordination

State capture must not occur in the realtime process call.

On Rack save/autosave:

- Request a fresh state snapshot through the host control layer.
- Suspend or synchronize with processing only at a safe block boundary if required.
- Keep audio interruption as short as possible.
- Write state files from the worker/control side.
- If fresh capture fails, retain the last valid state files and record a diagnostic warning rather than replacing them with empty/corrupt files.

### 20.4 Missing plugin restoration

If the stored plugin cannot be found:

- Preserve descriptor metadata.
- Preserve mappings.
- Preserve state files.
- Show `PLUGIN MISSING`.
- VST-FX passes dry audio.
- VST-Instrument outputs silence.
- Permit the user to locate a replacement path for the same class ID.
- Do not silently load a different plugin with the same display name.

### 20.5 Schema versioning

All JSON and patch-storage formats require a schema version.

Migration code must:

- Accept older known versions.
- Ignore unknown optional fields.
- Fail safely on unsupported future versions.
- Never reinterpret one parameter index as another parameter ID.

---

## 21. Status and Error Handling

Define explicit host states:

```cpp
enum class HostStatus {
    Empty,
    Scanning,
    Loading,
    Ready,
    Bypassed,
    EditorOpening,
    MissingPlugin,
    IncompatiblePlugin,
    LoadFailed,
    StateRestoreFailed,
    ProcessingFailed,
    Unloading
};
```

User-facing errors must include useful information without flooding logs:

- Plugin name.
- Vendor.
- Module path.
- Class ID.
- Operation that failed.
- Concise backend error.
- Rack sample rate.
- Block size.
- Operating system/architecture.

Provide `Copy diagnostic information` in the context menu.

### 21.1 Processing errors

If a process call returns failure:

- Increment a bounded failure counter.
- Do not log every block.
- After a small threshold of consecutive failures, mark the instance failed.
- Stop processing it.
- Transition to module-specific safe output.
- Require explicit retry or reload.

### 21.2 Crash limitation

Because MVP plugins run in Rack’s process, a segmentation fault, illegal instruction, deadlock, or fatal abort inside a plugin may terminate or freeze Rack. Catch C++ exceptions at host boundaries where practical, but do not claim protection from native plugin crashes.

Out-of-process hosting is a post-MVP hardening phase.

---

## 22. Module Context Menu

Shared entries:

```text
Plugin
  Open browser…
  Open editor
  Close editor
  Unload
  Retry / Reload
  Rescan plugins

Processing block size
  16
  32
  64
  128 ✓
  256
  512
  1024
  2048
  4096

Host tempo…
Editor
  Recenter
  Reset saved position

Diagnostics
  Copy plugin information
  Copy host diagnostics
```

VST-Instrument additional entry:

```text
Panic / All notes off
```

Optional developer-build entries:

```text
Log VST3 lifecycle
Validate realtime allocations
Dump parameter catalog
```

Do not expose developer diagnostics in normal release builds unless useful to end users.

---

## 23. Rack Model Registration

Create two independent Rack models backed by the shared implementation:

```cpp
Model* modelVstFx;
Model* modelVstInstrument;
```

Use stable slugs from the first public release. Do not rename slugs after users can save patches.

Because shipping names are not finalized, use temporary internal slugs only on development branches. Final slugs must be chosen before merging into the release branch.

---

## 24. Testing Strategy

### 24.1 Unit tests

Write tests for:

- 0–10 V to normalized parameter conversion.
- CV clamping below 0 V and above 10 V.
- Discrete parameter quantization.
- Parameter-ID mapping persistence.
- Missing-parameter resolution.
- Block bridge input/output indexing.
- Block-size changes.
- Mono-to-stereo adaptation.
- Rack voltage/audio float scaling.
- `V/OCT` to MIDI-note conversion.
- Pitch rounding at semitone boundaries.
- Pitch clamping to 0–127.
- Gate rising/falling detection.
- Pitch change while gate is high.
- Unique note-ID allocation.
- Velocity default and CV conversion.
- Pressure change suppression epsilon.
- Panic clearing every active note.
- State schema migration.
- Plugin descriptor deduplication.

### 24.2 Backend integration tests

Use official SDK test/sample plugins or locally built controlled fixtures:

- One stereo effect.
- One mono effect.
- One stereo instrument.
- One mono instrument.
- One plugin with no editor.
- One plugin with many parameters.
- One plugin with discrete parameters.
- One plugin with large state.
- One plugin bundle exposing multiple classes.

Verify:

- Load/configure/process/unload.
- Component/controller state round trip.
- Parameter learning.
- Manual parameter assignment.
- Native editor open/close.
- Editor close notification.
- Sample-rate change.
- Every supported block size.
- Plugin replacement transaction.

### 24.3 Rack integration tests

Create automated or scripted patches that verify:

#### VST-FX

- Left-only input reaches both plugin channels.
- Stereo input remains stereo.
- No-plugin pass-through.
- Bypass pass-through.
- Parameter CV reaches the mapped plugin parameter.
- Save/reload restores audible plugin state and mappings.

#### VST-Instrument

- Mono pitch/gate produces a note.
- Polyphonic pitch/gate produces up to sixteen notes.
- Velocity changes note-on velocity.
- Pressure emits changes only for active notes.
- Gate fall releases the correct note.
- Pitch changes under gate do not leave the old note active.
- Panic leaves no active notes.
- Save/reload restores plugin state and mappings without reopening the editor.

### 24.4 Stress tests

- Load/unload the same plugin 100 times.
- Alternate two plugins 100 times.
- Open/close editor repeatedly.
- Save/autosave while audio is running.
- Remove a module while its editor is open.
- Close Rack with plugins active.
- Change sample rate repeatedly.
- Change block size repeatedly.
- Disconnect/reconnect mapped CV.
- Load a patch with a missing plugin.
- Load a patch after moving the plugin bundle.
- Test plugins with 1,000+ exposed parameters.
- Run multiple host modules concurrently.

### 24.5 Manual compatibility matrix

Before MVP release, test at minimum:

- Five VST3 effects from multiple vendors.
- Five VST3 instruments from multiple vendors.
- At least one resizable editor.
- At least one fixed-size editor.
- At least one plugin with large preset/state data.
- At least one mono plugin.
- At least one plugin that lacks a native editor.
- At least one plugin whose UI emits parameter edits rapidly.

Record:

- Platform.
- Architecture.
- Plugin version.
- Load success.
- Audio success.
- Editor success.
- Mapping success.
- State restoration success.
- Known limitations.

---

## 25. Performance Requirements

- No per-sample heap allocation.
- No per-block unbounded heap allocation after plugin configuration.
- No filesystem or scanning work on the engine thread.
- No plugin loading, unloading, or destruction on the engine thread.
- No unbounded parameter queues.
- No unbounded event queues.
- Cap plugin-generated output parameter/event data per block and report overflow.
- Reuse audio, event, and parameter buffers.
- UI animation must not continuously invalidate large panel framebuffers.
- Idle modules with no loaded plugin should have minimal processing overhead.
- Parameter labels and host snapshots should update at a UI-friendly rate rather than every audio sample.

---

## 26. Security and Robustness Assumptions

Third-party plugins are native code loaded into Rack’s process. The MVP assumes installed plugins are trusted to the same degree as Rack plugins.

The host must still:

- Validate paths.
- Validate class IDs.
- Bound all copied strings.
- Bound event and parameter counts.
- Reject impossible bus/channel counts.
- Reject unreasonable state sizes when they would exhaust memory.
- Avoid following malformed metadata into unsafe buffer operations.
- Keep temporary state writes within the module’s patch-storage directory.
- Never execute arbitrary helper binaries discovered beside a plugin.

Full security isolation requires a separate plugin process and is deferred.

---

## 27. Implementation Milestones

### Milestone 0 — Dependency and standalone spike

Deliver:

- VST3 SDK integrated and pinned.
- Small non-Rack host test executable.
- Load a known effect.
- Enumerate parameters.
- Process a block.
- Save/restore state.
- Open the native editor on Linux.

Exit criteria:

- All lifecycle steps are understood.
- Build does not pull unnecessary SDK targets.
- A known plugin can process audio repeatedly without leaks.

### Milestone 1 — Shared catalog and backend

Deliver:

- `PluginCatalog`.
- Standard/custom path scanning.
- Cached plugin descriptors.
- `Vst3Backend`.
- Correct component/controller lifecycle.
- Stereo and mono bus negotiation.
- Immutable parameter catalog.

Exit criteria:

- Backend integration tests pass outside the final Rack modules.

### Milestone 2 — VST-FX audible MVP

Deliver:

- VST-FX Rack module.
- Stereo input/output.
- Right normalization.
- Block bridge.
- Plugin browser.
- Asynchronous load/replacement.
- Bypass.
- Safe no-plugin and failure pass-through.

Exit criteria:

- Multiple effects process live Rack audio.
- No plugin lifecycle work occurs in `Module::process()`.

### Milestone 3 — Native editor and mapping widgets

Deliver:

- Large editor eye.
- Native editor window.
- Sixteen `PluginParameterSlotWidget` instances.
- Learn mode.
- Manual searchable parameter picker.
- Direct 0–10 V mapping.
- Parameter-ID persistence.

Exit criteria:

- User can load an effect, map parameters by wiggling its UI, close the editor, and control it entirely from Rack CV.

### Milestone 4 — VST-Instrument

Deliver:

- V/OCT, gate, velocity, and pressure inputs.
- Sample-offset note event queue.
- Up to sixteen voices.
- Stereo output.
- Panic.
- Bypass mute behavior.
- Instrument-compatible browser filtering.

Exit criteria:

- Polyphonic Rack sequencing can play several tested instruments without stuck notes.

### Milestone 5 — Persistence and hardening

Deliver:

- Component/controller patch-storage files.
- Async safe state capture.
- Full patch restore.
- Missing plugin behavior.
- Editor geometry persistence.
- Diagnostic copy action.
- Stress tests.
- Compatibility matrix.

Exit criteria:

- A patch can be saved, Rack restarted, and both effect and instrument instances restored audibly with their mappings intact.

### Milestone 6 — Platform expansion

Post-MVP:

- Windows editor/platform adapter.
- macOS editor/platform adapter.
- Platform compatibility testing.
- Release packaging and documentation.

---

## 28. MVP Acceptance Criteria

The work is complete only when all of the following are true.

### Shared

- [ ] Both modules use one shared VST3 backend.
- [ ] VST3 SDK dependency is reproducible and license notices are included.
- [ ] Plugin scanning never runs on Rack’s engine thread.
- [ ] Plugin loading/unloading/destruction never occurs on Rack’s engine thread.
- [ ] Plugin browser lists individual plugin classes.
- [ ] Plugin selection is transactional.
- [ ] Large eye button opens and closes the native plugin editor.
- [ ] Closing the native window updates the eye state.
- [ ] Exactly sixteen mapping slots are visible.
- [ ] Each jack has a co-located mapping element and dynamic label.
- [ ] Clicking a mapping element enters learn mode.
- [ ] Moving a plugin control assigns the correct stable `ParamID`.
- [ ] Right-click allows manual assignment and clearing.
- [ ] Connected 0–10 V CV controls the normalized parameter range.
- [ ] Discrete parameters are quantized correctly.
- [ ] Unpatched mapped slots do not overwrite plugin UI changes.
- [ ] Missing parameters are shown as missing, never remapped by index.
- [ ] Block size is selectable and saved.
- [ ] Host tempo is saved.
- [ ] Component and controller states are saved in patch storage.
- [ ] Mappings and host metadata are saved in JSON.
- [ ] Patches restore without automatically opening editor windows.
- [ ] Audio processing contains no known filesystem access or plugin lifecycle work.
- [ ] Failure states are visible and diagnostically useful.

### VST-FX

- [ ] Stereo effects process Rack audio.
- [ ] Mono effects adapt correctly.
- [ ] Right input normalizes from left.
- [ ] No-plugin state passes audio through.
- [ ] Failed-plugin state passes audio through.
- [ ] Bypass passes dry audio with a short transition.
- [ ] Plugin state and mappings restore audibly after Rack restart.

### VST-Instrument

- [ ] `0 V` pitch with a gate produces middle C.
- [ ] Pitch is rounded to the nearest semitone.
- [ ] Polyphonic cables produce up to sixteen note streams.
- [ ] Gate falls release the correct note IDs.
- [ ] Pitch changes under a held gate replace the old note cleanly.
- [ ] Velocity defaults to approximately 100/127 when unpatched.
- [ ] Velocity CV affects note-on velocity.
- [ ] Pressure CV produces poly-pressure events for active notes.
- [ ] Panic clears every active note.
- [ ] Unload, reset, sample-rate change, and module deletion cannot leave tracked notes active.
- [ ] No-plugin, failed-plugin, and bypass states output silence.
- [ ] Plugin state and mappings restore audibly after Rack restart.

---

## 29. Deferred Evolution

The architecture should leave clear seams for:

- CLAP backend.
- Format-neutral public module names.
- Worker-thread audio mode.
- Out-of-process scanning.
- Out-of-process plugin processing and crash recovery.
- Sidechain and additional buses.
- Multi-output instruments.
- Host-CV/MIDI output expanders.
- Clock/transport inputs.
- Tempo derived from Rack clock.
- MPE and Note Expression.
- Continuous pitch bend and microtonal tuning.
- Per-slot range, offset, attenuation, polarity, and slew.
- Sample-accurate parameter automation.
- Generic editor for headless plugins.
- Preset browser and preset files.
- Plugin favorites and recent list.
- Automatic latency compensation.
- Wet/dry controls.
- Host chains.
- Shared plugin-process pools.

None of these should be partially implemented in the MVP unless required to make the core behavior correct.

---

## 30. Final Implementation Directive

Build the smallest complete host, not a miniature DAW.

The first release succeeds when a user can:

1. Add either host module.
2. Load a compatible VST3 effect or instrument.
3. Open its native UI with the eye.
4. Click the tiny mapping element beneath any of sixteen CV jacks.
5. Move a plugin control.
6. Close the plugin UI.
7. Modulate that control from Rack.
8. Save the patch.
9. Reopen the patch with the plugin state and mappings restored.

Every architectural choice should protect that path: simple on the panel, correct in the host, bounded on the audio thread, and extensible beneath the surface.
