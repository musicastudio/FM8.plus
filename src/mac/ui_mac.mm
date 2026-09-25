#include "ui_mac.h"
#include "core_mac.h"
#include "../core/settings.h"
#import <Cocoa/Cocoa.h>
#include <cmath>

// Records which item was picked; NSMenu reports it through an action.
@interface FM8PlusMenuTarget : NSObject
@property(nonatomic) NSInteger picked;
- (void)pick:(NSMenuItem*)item;
@end
@implementation FM8PlusMenuTarget
- (void)pick:(NSMenuItem*)item { self.picked = item.tag; }
@end

namespace fm8plus::macui {
namespace {

enum {
    ID_MORPH_OFF = 1000, ID_MORPH_CC0 = 1001,
    ID_ARP_INT = 2000, ID_ARP_CLONE, ID_ARP_MIDI,
    ID_TEMPO_OFF = 3000,
    ID_GAIN_OFF = 4000,
    ID_SCALE_0 = 5000,
    ID_ABOUT_FM8 = 6000, ID_ABOUT_PLUS,
};
constexpr float kScales[] = {1.0f, 2.0f, 3.0f, 4.0f};
NSString* const kScaleLabels[] = {@"1x (off)", @"2x", @"3x", @"4x"};
NSString* const kProjectUrl = @"https://github.com/musicastudio/FM8.plus";

const char* ccName(int cc) {
    switch (cc) {
        case 0: return "Bank Select"; case 1: return "Mod Wheel"; case 2: return "Breath";
        case 4: return "Foot"; case 5: return "Portamento Time"; case 6: return "Data Entry";
        case 7: return "Volume"; case 8: return "Balance"; case 10: return "Pan";
        case 11: return "Expression"; case 64: return "Sustain"; case 65: return "Portamento";
        case 66: return "Sostenuto"; case 67: return "Soft Pedal"; case 71: return "Resonance";
        case 74: return "Cutoff"; case 84: return "Portamento Ctrl"; case 91: return "Reverb";
        case 93: return "Chorus"; case 94: return "Detune"; case 95: return "Phaser";
        case 120: return "All Sound Off"; case 121: return "Reset Controllers"; case 123: return "All Notes Off";
        default: return nullptr;
    }
}

NSMenuItem* add(NSMenu* m, NSString* title, NSInteger tag, bool checked, bool enabled, FM8PlusMenuTarget* t) {
    NSMenuItem* it = [m addItemWithTitle:title action:(enabled ? @selector(pick:) : nil) keyEquivalent:@""];
    it.tag = tag;
    it.target = enabled ? t : nil;
    it.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    it.enabled = enabled;
    return it;
}

NSMenu* submenu(NSMenu* parent, NSString* title, bool enabled) {
    NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
    sub.autoenablesItems = NO;
    NSMenuItem* it = [parent addItemWithTitle:title action:nil keyEquivalent:@""];
    it.submenu = sub;
    it.enabled = enabled;
    return sub;
}
} // namespace

void showMenu(InstanceState* st, const ScaleHost& scale) {
    @autoreleasepool {
        FM8PlusMenuTarget* t = [FM8PlusMenuTarget new];
        t.picked = 0;
        NSMenu* m = [[NSMenu alloc] initWithTitle:@"FM8.plus"];
        m.autoenablesItems = NO;
        add(m, @"FM8.plus", 0, false, false, t);
        [m addItem:[NSMenuItem separatorItem]];

        const bool midi = Core::midiFeaturesAvailable();
        const int16_t cc = st->morphCc.load();
        NSMenu* morph = submenu(m, @"Morph Rotate Control", Core::aboutReady(*st));
        add(morph, @"Off", ID_MORPH_OFF, cc < 0, true, t);
        [morph addItem:[NSMenuItem separatorItem]];
        for (int n = 0; n < 128; ++n) {
            const char* nm = ccName(n);
            add(morph, nm ? [NSString stringWithFormat:@"CC %d (%s)", n, nm] : [NSString stringWithFormat:@"CC %d", n],
                ID_MORPH_CC0 + n, cc == n, true, t);
        }

        const auto mode = (ArpMode)st->arpMode.load();
        NSMenu* arp = submenu(m, @"Arpeggiator MIDI out", midi);
        add(arp, @"Internal", ID_ARP_INT, mode == ArpMode::Internal, true, t);
        add(arp, @"Clone to MIDI", ID_ARP_CLONE, mode == ArpMode::CloneToMidi, true, t);
        add(arp, @"MIDI only (FM8 silent)", ID_ARP_MIDI, mode == ArpMode::MidiOnly, true, t);

        NSString* const tempoLabels[] = {@"Off", @"0.25x Host", @"0.5x Host", @"2x Host", @"4x Host", @"Custom"};
        const uint8_t tm = st->tempoMode.load();
        NSMenu* tempo = submenu(m, @"Tempo Override", true);
        for (int i = 0; i < 6; ++i) add(tempo, tempoLabels[i], ID_TEMPO_OFF + i, tm == i, true, t);

        const int8_t db = st->gainDb.load();
        NSMenu* gain = submenu(m, @"Increase Gain", true);
        add(gain, @"Off", ID_GAIN_OFF, db == 0, true, t);
        for (int n = 1; n <= 10; ++n) add(gain, [NSString stringWithFormat:@"+%d dB", n], ID_GAIN_OFF + n, db == n, true, t);

        if (scale.apply) {
            NSMenu* sc = submenu(m, @"GUI Scale", true);
            for (int i = 0; i < 4; ++i)
                add(sc, kScaleLabels[i], ID_SCALE_0 + i, std::fabs(Core::guiScale() - kScales[i]) < 0.01f, true, t);
        }

        [m addItem:[NSMenuItem separatorItem]];
        add(m, @"About FM8", ID_ABOUT_FM8, false, Core::aboutReady(*st), t);
        add(m, @"About FM8.plus", ID_ABOUT_PLUS, false, true, t);

        [m popUpMenuPositioningItem:nil atLocation:[NSEvent mouseLocation] inView:nil];
        const NSInteger cmd = t.picked;

        if (cmd == ID_MORPH_OFF) { st->morphCc.store(-1); settings::setMorphCcDefault(-1); }
        else if (cmd >= ID_MORPH_CC0 && cmd < ID_MORPH_CC0 + 128) {
            st->morphCc.store((int16_t)(cmd - ID_MORPH_CC0)); settings::setMorphCcDefault((int)(cmd - ID_MORPH_CC0));
        }
        else if (cmd >= ID_ARP_INT && cmd <= ID_ARP_MIDI) {
            st->arpMode.store((uint8_t)(cmd - ID_ARP_INT)); st->pendingFlush.store(true);
            settings::setArpModeDefault((int)(cmd - ID_ARP_INT));
        }
        else if (cmd >= ID_TEMPO_OFF && cmd <= ID_TEMPO_OFF + 5) st->tempoMode.store((uint8_t)(cmd - ID_TEMPO_OFF));
        else if (cmd >= ID_GAIN_OFF && cmd <= ID_GAIN_OFF + 10) st->gainDb.store((int8_t)(cmd - ID_GAIN_OFF));
        else if (cmd >= ID_SCALE_0 && cmd < ID_SCALE_0 + 4) {
            Core::setGuiScale(kScales[cmd - ID_SCALE_0]);
            settings::setGuiScale(Core::guiScale());
            scale.apply(scale.ctx, Core::guiScale());
        }
        else if (cmd == ID_ABOUT_FM8) Core::showAboutMac(st);
        else if (cmd == ID_ABOUT_PLUS) [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kProjectUrl]];
    }
}

} // namespace fm8plus::macui
