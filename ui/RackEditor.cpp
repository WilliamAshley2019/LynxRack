#include "RackEditor.h"

namespace lynx
{
namespace
{
constexpr int toolbarRow = 30;
const int lengths[] = { 4, 8, 12, 16, 24, 32, 48, 64 };
const std::pair<const char*, int> snaps[] = { { "1/4", ppq }, { "1/8", ppq / 2 }, { "1/16", ppq / 4 }, { "1/32", ppq / 8 }, { "Off", 1 } };
}

RackEditor::RackEditor (RackProcessor& p)
    : juce::AudioProcessorEditor (p), rack (p), strip (p), roll (p)
{
    setLookAndFeel (&lnf);
    setResizable (true, true);
    setResizeLimits (900, 560, 2400, 1600);
    setSize (1180, 760);

    for (auto* c : std::initializer_list<juce::Component*> { &patternLabel, &swingLabel, &snapLabel, &modeLabel, &tempoLabel, &status,
                                                              &patternBox, &lengthBox, &snapBox, &modeBox, &swingSlider, &tempoSlider,
                                                              &undoButton, &fitButton, &rewindButton, &playButton, &stopButton,
                                                              &panicButton, &scanButton, &viewport, &roll })
        addAndMakeVisible (*c);

    for (auto* l : { &patternLabel, &swingLabel, &snapLabel, &modeLabel, &tempoLabel })
        l->setColour (juce::Label::textColourId, theme::dim);
    status.setColour (juce::Label::textColourId, theme::dim);
    status.setJustificationType (juce::Justification::centredRight);

    for (int i = 1; i <= maxPatterns; ++i)
        patternBox.addItem (juce::String (i), i);
    modeBox.addItem ("Follow host", 1);
    modeBox.addItem ("Internal", 2);
    for (auto n : lengths)
        lengthBox.addItem (juce::String (n) + " steps", n);
    for (int i = 0; i < (int) std::size (snaps); ++i)
        snapBox.addItem (snaps[i].first, i + 1);
    snapBox.setSelectedId (3, juce::dontSendNotification);

    auto& apvts = rack.getParameters();
    patternAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "pattern", patternBox);
    modeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, "mode", modeBox);
    swingAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "swing", swingSlider);
    tempoAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "tempo", tempoSlider);

    swingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    tempoSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    tempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    tempoSlider.setTooltip ("Tempo used when the rack runs on its own transport");

    swingSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    swingSlider.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
    swingSlider.updateText();

    lengthBox.onChange = [this] {
        const int steps = lengthBox.getSelectedId();
        const int pat = rack.activePattern();
        if (steps <= 0 || rack.getEngine().song().patterns[(size_t) pat].steps == steps)
            return;
        rack.getEngine().beginEdit (pat);
        rack.getEngine().editPattern (pat, [steps] (Pattern& pattern) { pattern.steps = steps; });
        roll.fitPattern();
    };
    snapBox.onChange = [this] {
        const int index = snapBox.getSelectedId() - 1;
        if (index >= 0 && index < (int) std::size (snaps))
            roll.setSnap (snaps[index].second);
    };
    undoButton.onClick = [this] { rack.getEngine().undo(); };
    fitButton.onClick = [this] { roll.fitPattern(); };
    rewindButton.onClick = [this] { rack.getEngine().rewindInternal(); };
    playButton.onClick = [this] { rack.getEngine().setInternalPlaying (true); };
    stopButton.onClick = [this] { rack.getEngine().setInternalPlaying (false); };
    panicButton.onClick = [this] { rack.getEngine().panic(); };
    scanButton.onClick = [this] { showScanMenu(); };

    viewport.setViewedComponent (&strip, false);
    viewport.setScrollBarsShown (true, false);

    strip.onSelect = [this] (int ch) { selectChannel (ch); };
    strip.onSlotMenu = [this] (int ch) { selectChannel (ch); showSlotMenu (ch); };
    strip.onToggleGui = [this] (int ch) { rack.getHost().toggleEditor (ch); };

    rack.getEngine().addChangeListener (this);
    refreshAll();
    rack.getHost().warmUp();
    scanStatus = rack.getHost().libraryHint();
    roll.fitPattern();
    roll.centreOnNotes();
    startTimerHz (30);
}

RackEditor::~RackEditor()
{
    stopTimer();
    rack.getEngine().removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void RackEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
}

void RackEditor::resized()
{
    auto r = getLocalBounds().reduced (8);

    auto rowA = r.removeFromTop (toolbarRow);
    patternLabel.setBounds (rowA.removeFromLeft (52));
    patternBox.setBounds (rowA.removeFromLeft (64));         rowA.removeFromLeft (6);
    lengthBox.setBounds (rowA.removeFromLeft (96));          rowA.removeFromLeft (14);
    swingLabel.setBounds (rowA.removeFromLeft (44));
    swingSlider.setBounds (rowA.removeFromLeft (150));       rowA.removeFromLeft (14);
    snapLabel.setBounds (rowA.removeFromLeft (36));
    snapBox.setBounds (rowA.removeFromLeft (64));            rowA.removeFromLeft (14);
    undoButton.setBounds (rowA.removeFromLeft (54));         rowA.removeFromLeft (4);
    fitButton.setBounds (rowA.removeFromLeft (44));
    status.setBounds (rowA);

    r.removeFromTop (4);
    auto rowB = r.removeFromTop (toolbarRow);
    modeLabel.setBounds (rowB.removeFromLeft (66));
    modeBox.setBounds (rowB.removeFromLeft (110));           rowB.removeFromLeft (10);
    tempoLabel.setBounds (rowB.removeFromLeft (48));
    tempoSlider.setBounds (rowB.removeFromLeft (190));       rowB.removeFromLeft (14);
    rewindButton.setBounds (rowB.removeFromLeft (40));       rowB.removeFromLeft (4);
    playButton.setBounds (rowB.removeFromLeft (56));         rowB.removeFromLeft (4);
    stopButton.setBounds (rowB.removeFromLeft (56));         rowB.removeFromLeft (4);
    panicButton.setBounds (rowB.removeFromLeft (56));
    scanButton.setBounds (rowB.removeFromRight (110));

    r.removeFromTop (6);
    const int rackHeight = juce::jlimit (RackRow::height * 4, RackRow::height * maxChannels, (int) (r.getHeight() * 0.38f));
    viewport.setBounds (r.removeFromTop (rackHeight));
    strip.setSize (viewport.getMaximumVisibleWidth(), RackRow::height * maxChannels);

    r.removeFromTop (6);
    roll.setBounds (r);
}

void RackEditor::refreshAll()
{
    shownPattern = rack.activePattern();
    strip.refresh();

    const int steps = rack.getEngine().song().patterns[(size_t) shownPattern].steps;
    if (lengthBox.getSelectedId() != steps)
    {
        if (lengthBox.indexOfItemId (steps) >= 0)
            lengthBox.setSelectedId (steps, juce::dontSendNotification);
        else
            lengthBox.setText (juce::String (steps) + " steps", juce::dontSendNotification);
    }
}

void RackEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
    roll.repaint();
}

void RackEditor::selectChannel (int channel)
{
    rack.selectChannel (channel);
    strip.refresh();
    roll.patternOrChannelChanged();
}

void RackEditor::timerCallback()
{
    if (rack.activePattern() != shownPattern)
    {
        refreshAll();
        roll.patternOrChannelChanged();
        roll.fitPattern();
        roll.centreOnNotes();
    }

    auto& engine = rack.getEngine();
    const auto tick = engine.playheadTick();
    const bool playing = engine.isPlayingNow();
    strip.setPlayStep (playing ? (int) (tick / ticksPerStep) : -1);
    roll.setPlayhead (tick, playing);
    strip.tickMeters();

    if (playing != shownPlaying)
    {
        shownPlaying = playing;
        playButton.setToggleState (playing, juce::dontSendNotification);
    }

    juce::String text = juce::String (engine.currentBpm(), 1) + " bpm";
    if (scanStatus.isNotEmpty())
        text = scanStatus + "   " + text;
    status.setText (text, juce::dontSendNotification);
}

void RackEditor::runScan (bool fromFl)
{
    auto& host = rack.getHost();

    if (host.isScanning())
        return;

    scanButton.setEnabled (false);
    auto report = [weak = juce::WeakReference<RackEditor> (this)] (const juce::String& text, bool finished) {
        if (weak == nullptr)
            return;
        weak->scanStatus = text;
        if (finished)
        {
            weak->scanButton.setEnabled (true);
            weak->strip.refresh();          // channels waiting for a library that has just been filled in
        }
    };

    if (fromFl)
        host.importFromFl (report);
    else
        host.scanFolders (report);

    if (! host.isScanning())                // could not start (e.g. LemonScan missing): the report already said why
        scanButton.setEnabled (true);
}

void RackEditor::showScanMenu()
{
    auto& host = rack.getHost();
    const auto quarantined = host.quarantinedCount();

    juce::PopupMenu menu;
    menu.addItem (1, "Scan plugin folders (VST3)", ! host.isScanning());
    menu.addItem (2, "Import from FL Studio plugin database", ! host.isScanning());
    menu.addSeparator();
    menu.addItem (3, "Release quarantined plugins (" + juce::String (quarantined) + ")", quarantined > 0);
    menu.addItem (4, "Open library folder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scanButton),
                        [this, weak = juce::WeakReference<RackEditor> (this)] (int result) {
        if (weak == nullptr)
            return;

        if (result == 1)       runScan (false);
        else if (result == 2)  runScan (true);
        else if (result == 3)  { rack.getHost().releaseQuarantine(); scanStatus = "Quarantine cleared"; strip.refresh(); }
        else if (result == 4)  rack.getHost().dataFolder().startAsProcess();
    });
}

void RackEditor::renameChannel (int channel)
{
    auto* w = new juce::AlertWindow ("Rename channel", {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", rack.getEngine().song().channels[(size_t) channel].name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, channel, weak = juce::WeakReference<RackEditor> (this)] (int result) {
        if (result == 1 && weak != nullptr)
        {
            const auto name = w->getTextEditorContents ("name").trim();
            if (name.isNotEmpty())
                rack.getEngine().editChannel (channel, [&] (ChannelInfo& c) { c.name = name; });
        }
    }), true);
}

void RackEditor::showSlotMenu (int channel)
{
    auto& engine = rack.getEngine();
    const auto& info = engine.song().channels[(size_t) channel];

    juce::PopupMenu menu;
    menu.addSubMenu ("Load plugin", rack.getHost().makePluginMenu());
    menu.addItem (1, "Unload plugin", engine.channelPlugin (channel) != nullptr);
    menu.addItem (2, "Rename...");
    menu.addSeparator();

    juce::PopupMenu transpose;
    for (int semis : { -24, -12, -7, -5, -1, 0, 1, 5, 7, 12, 24 })
        transpose.addItem (100 + semis + 48, (semis > 0 ? "+" : "") + juce::String (semis) + " semitones", true, info.transpose == semis);
    menu.addSubMenu ("Transpose", transpose);

    juce::PopupMenu midiOut;
    midiOut.addItem (300, "Off (hosted plugin only)", true, info.midiOut == 0);
    for (int c = 1; c <= 16; ++c)
        midiOut.addItem (300 + c, "Also send as MIDI channel " + juce::String (c), true, info.midiOut == c);
    menu.addSubMenu ("MIDI output to host", midiOut);

    juce::PopupMenu stepKey;
    for (auto [label, key] : { std::pair { "C2 (36)", 36 }, { "C3 (48)", 48 }, { "C4 (60)", 60 }, { "C5 (72)", 72 } })
        stepKey.addItem (400 + key, label, true, info.stepKey == key);
    menu.addSubMenu ("Step button key", stepKey);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (180),
                        [this, channel, weak = juce::WeakReference<RackEditor> (this)] (int result) {
        if (weak == nullptr || result == 0)
            return;

        auto& host = rack.getHost();
        auto& eng = rack.getEngine();

        if (juce::PluginDescription desc; host.descriptionForMenuResult (result, desc))
        {
            host.loadPlugin (channel, desc, [weak, name = desc.name] (const juce::String& error) {
                if (weak != nullptr)
                {
                    if (error.isNotEmpty())
                        weak->scanStatus = "Load failed: " + error;
                    weak->strip.refresh();
                }
            });
        }
        else if (result == 1)  host.unloadPlugin (channel);
        else if (result == 2)  renameChannel (channel);
        else if (result >= 124 && result <= 172) eng.editChannel (channel, [result] (ChannelInfo& c) { c.transpose = result - 148; });
        else if (result >= 300 && result <= 316) eng.editChannel (channel, [result] (ChannelInfo& c) { c.midiOut = result - 300; });
        else if (result >= 400 && result < 600) eng.editChannel (channel, [result] (ChannelInfo& c) { c.stepKey = result - 400; });
    });
}
} // namespace lynx
