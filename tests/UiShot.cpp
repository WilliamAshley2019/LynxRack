// Renders the editor to a PNG (software renderer): a layout smoke test that needs no real window.
#include <juce_audio_utils/juce_audio_utils.h>

#include "../core/RackProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    lynx::RackProcessor proc;
    proc.prepareToPlay (48000.0, 512);
    auto& engine = proc.getEngine();

    auto add = [&] (lynx::Pattern& p, int ch, int step, int key, int lenSteps = 1, float vel = 0.8f) {
        lynx::Note n;
        n.start = (std::int64_t) step * lynx::ticksPerStep;
        n.length = (std::int64_t) lenSteps * lynx::ticksPerStep;
        n.channel = ch; n.key = key; n.velocity = vel;
        p.add (n);
    };

    engine.editPattern (0, [&] (lynx::Pattern& p) {
        p.steps = 32;
        for (int s = 0; s < 32; s += 4) add (p, 0, s, 36);
        for (int s = 4; s < 32; s += 8) add (p, 1, s, 38);
        for (int s = 2; s < 32; s += 4) add (p, 2, s, 42, 1, 0.5f);
        const int line[] = { 57, 60, 64, 62, 60, 57, 55, 59 };
        for (int i = 0; i < 8; ++i) add (p, 3, i * 4, line[i], 2 + (i % 2), 0.55f + 0.05f * (float) i);
        add (p, 3, 2, 69, 1, 0.9f); add (p, 3, 18, 72, 3, 0.7f);
    });
    for (int i = 0; i < 4; ++i)
        engine.editChannel (i, [i] (lynx::ChannelInfo& c) { c.name = juce::StringArray { "Kick", "Snare", "Hats", "Lead" }[i]; });

    proc.selectChannel (3);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (1180, 760);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    editor->resized();

    const auto path = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "editor.png");
    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    path.deleteFile();
    juce::FileOutputStream out (path);
    juce::PNGImageFormat().writeImageToStream (image, out);
    std::cout << "wrote " << path.getFullPathName() << "\n";
    editor.reset();
    return 0;
}
