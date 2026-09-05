#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

static const char *text(NSString *value)
{
    return value ? [value UTF8String] : "(none)";
}

static void describe_controller(GCController *controller)
{
    fprintf(stderr, "controller: vendor=%s category=%s attached=%s\n",
            text([controller vendorName]), text([controller productCategory]),
            [controller isAttachedToDevice] ? "yes" : "no");

    GCExtendedGamepad *gamepad = [controller extendedGamepad];
    fprintf(stderr,
            "extended: menu=%p options=%p home=%p\n",
            [gamepad buttonMenu], [gamepad buttonOptions], [gamepad buttonHome]);

    GCPhysicalInputProfile *profile = [controller physicalInputProfile];
    NSArray<NSString *> *names = [[[profile buttons] allKeys]
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *name in names) {
        GCControllerButtonInput *button = [[profile buttons] objectForKey:name];
        fprintf(stderr,
                "button: key=%s name=%s unmapped=%s aliases=%s "
                "gesture=%s state=%ld value=%.3f pressed=%s\n",
                text(name), text([button localizedName]),
                text([button unmappedLocalizedName]),
                text([[[button aliases] allObjects] componentsJoinedByString:@","]),
                [button isBoundToSystemGesture] ? "yes" : "no",
                (long)[button preferredSystemGestureState], [button value],
                [button isPressed] ? "yes" : "no");
    }

    [gamepad setValueChangedHandler:^(GCExtendedGamepad *changed,
                                      GCControllerElement *element) {
        NSArray<NSArray *> *known = @[
            @[@"A", [changed buttonA]], @[@"B", [changed buttonB]],
            @[@"X", [changed buttonX]], @[@"Y", [changed buttonY]],
            @[@"L1", [changed leftShoulder]],
            @[@"R1", [changed rightShoulder]],
            @[@"L2", [changed leftTrigger]],
            @[@"R2", [changed rightTrigger]],
            @[@"L3", [changed leftThumbstickButton] ?: [NSNull null]],
            @[@"R3", [changed rightThumbstickButton] ?: [NSNull null]],
            @[@"Menu", [changed buttonMenu]],
            @[@"Options", [changed buttonOptions] ?: [NSNull null]],
            @[@"Home", [changed buttonHome] ?: [NSNull null]],
        ];
        for (NSArray *entry in known) {
            if ([entry objectAtIndex:1] == element) {
                GCControllerButtonInput *button = [entry objectAtIndex:1];
                fprintf(stderr, "event: %-7s value=%.3f pressed=%s\n",
                        text([entry objectAtIndex:0]), [button value],
                        [button isPressed] ? "yes" : "no");
                break;
            }
        }
    }];
}

int main(void)
{
    @autoreleasepool {
        if (@available(macOS 11.3, *)) {
            [GCController setShouldMonitorBackgroundEvents:YES];
        }
        fprintf(stderr,
                "Controller probe listening for 45 seconds; press Options, "
                "Share, PS, touchpad, and the four face buttons.\n");

        NSMutableSet *seen = [NSMutableSet set];
        NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:45.0];
        while ([deadline timeIntervalSinceNow] > 0.0) {
            for (GCController *controller in [GCController controllers]) {
                NSValue *identity = [NSValue valueWithPointer:controller];
                if (![seen containsObject:identity]) {
                    [seen addObject:identity];
                    describe_controller(controller);
                }
            }
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        fprintf(stderr, "Controller probe finished (%lu controller%s).\n",
                (unsigned long)[seen count], [seen count] == 1 ? "" : "s");
    }
    return 0;
}
