/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

/**
 * @brief Cocoa app entry point for Sushi with GUI support.
 *        Uses NSApplication run loop so that dispatch_get_main_queue()
 *        works for main-thread UI operations (VST3 editor hosting).
 * @Copyright 2026 Oliver Larkin
 */

#include <iostream>
#include <csignal>

#import <Cocoa/Cocoa.h>

#include "elklog/static_logger.h"

#include "sushi/utils.h"
#include "sushi/parameter_dump.h"
#include "sushi/portaudio_devices_dump.h"
#include "sushi/coreaudio_devices_dump.h"
#include "sushi/sushi.h"
#include "sushi/terminal_utilities.h"
#include "sushi/standalone_factory.h"
#include "sushi/offline_factory.h"
#include "sushi_status_bar.h"

using namespace sushi;

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("main");

static void error_exit(const std::string& message, sushi::Status status)
{
    std::cerr << message << std::endl;
    int error_code = static_cast<int>(status);
    std::exit(error_code);
}

static std::unique_ptr<Sushi> start_sushi(SushiOptions options)
{
    std::unique_ptr<FactoryInterface> factory;

    if (options.frontend_type == FrontendType::DUMMY ||
        options.frontend_type == FrontendType::OFFLINE)
    {
        factory = std::make_unique<OfflineFactory>();
    }
    else if (options.frontend_type == FrontendType::JACK ||
             options.frontend_type == FrontendType::XENOMAI_RASPA ||
             options.frontend_type == FrontendType::APPLE_COREAUDIO ||
             options.frontend_type == FrontendType::PORTAUDIO)
    {
        factory = std::make_unique<StandaloneFactory>();
    }
    else
    {
        error_exit("Invalid frontend configuration. Reactive, or None, are not supported when standalone.",
                   Status::FRONTEND_IS_INCOMPATIBLE_WITH_STANDALONE);
    }

    auto [sushi, status] = factory->new_instance(options);

    if (status == Status::FAILED_OSC_FRONTEND_INITIALIZATION)
    {
        error_exit("Instantiating OSC server on port " + std::to_string(options.osc_server_port) + " failed.",
                   status);
    }
    else if (status != Status::OK)
    {
        auto message = to_string(status);
        if (status == Status::FAILED_INVALID_FILE_PATH)
        {
            message.append(options.config_filename);
        }

        error_exit(message, status);
    }

    if (options.enable_parameter_dump)
    {
        std::cout << sushi::generate_processor_parameter_document(sushi->controller());
        std::cout << "Parameter dump completed - exiting." << std::endl;
        std::exit(EXIT_SUCCESS);
    }
    else
    {
        std::cout << "SUSHI - Copyright 2017-2023 Elk Audio AB, Stockholm" << std::endl;
        std::cout << "SUSHI is licensed under the Affero GPL 3.0. Source code is available at github.com/elk-audio" << std::endl;
    }

    auto start_status = sushi->start();

    if (start_status == Status::OK)
    {
        return std::move(sushi);
    }
    else if (start_status == Status::FAILED_TO_START_RPC_SERVER)
    {
        sushi.reset();
        error_exit("Failure starting gRPC server on address " + options.grpc_listening_address, status);
    }

    return nullptr;
}

static void pipe_signal_handler([[maybe_unused]] int sig)
{
    ELKLOG_LOG_INFO("Pipe signal received and ignored: {}", sig);
}

@protocol SushiTerminationControlling <NSObject>
- (void)requestAuthorizedTermination:(id)sender;
@end

@interface SushiAppDelegate : NSObject <NSApplicationDelegate, SushiTerminationControlling, NSWindowDelegate>
{
    std::unique_ptr<Sushi> _sushi;
    SushiStatusBar* _statusBar;
    int _argc;
    char** _argv;
    BOOL _restrictAppleQuit;
    BOOL _allowTermination;
    BOOL _showingQuitAlert;
    NSPanel* _quitWarningPanel;
    id _keyDownMonitor;
}

- (instancetype)initWithArgc:(int)argc argv:(char**)argv;
- (void)requestAuthorizedTermination:(id)sender;
- (void)showQuitViaStatusBarPanel;
- (void)closeQuitViaStatusBarPanel:(id)sender;

@end

static void exit_on_signal([[maybe_unused]] int sig)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        id<SushiTerminationControlling> delegate = (id<SushiTerminationControlling>)[NSApp delegate];
        if (delegate != nil)
        {
            [delegate requestAuthorizedTermination:nil];
        }
        else
        {
            [NSApp terminate:nil];
        }
    });
}

@implementation SushiAppDelegate

- (instancetype)initWithArgc:(int)argc argv:(char**)argv
{
    self = [super init];
    if (self)
    {
        _argc = argc;
        _argv = argv;
        _restrictAppleQuit = NO;
        _allowTermination = NO;
        _showingQuitAlert = NO;
        _quitWarningPanel = nil;
        _keyDownMonitor = nil;
    }
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    (void)notification;

    int argc = _argc;
    char** argv = _argv;

    if (argc > 0)
    {
        argc--;
        argv++;
    }

    SushiOptions options;

    auto option_status = parse_options(argc, argv, options);
    if (option_status == ParseStatus::ERROR)
    {
        [NSApp terminate:nil];
        return;
    }
    else if (option_status == ParseStatus::MISSING_ARGUMENTS)
    {
        [NSApp terminate:nil];
        return;
    }
    else if (option_status == ParseStatus::EXIT)
    {
        [NSApp terminate:nil];
        return;
    }

    init_logger(options);

    if (options.enable_audio_devices_dump)
    {
        if (options.frontend_type == FrontendType::PORTAUDIO)
        {
#ifdef SUSHI_BUILD_WITH_PORTAUDIO
            std::cout << sushi::generate_portaudio_devices_info_document() << std::endl;
#else
            std::cerr << "SUSHI not built with Portaudio support, cannot dump devices." << std::endl;
#endif
        }
        else if (options.frontend_type == FrontendType::APPLE_COREAUDIO)
        {
#ifdef SUSHI_BUILD_WITH_APPLE_COREAUDIO
            std::cout << sushi::generate_coreaudio_devices_info_document() << std::endl;
#else
            std::cerr << "SUSHI not built with Apple CoreAudio support, cannot dump devices." << std::endl;
#endif
        }
        else
        {
            std::cout << "No frontend specified or specified frontend not supported (please specify ." << std::endl;
        }
        [NSApp terminate:nil];
        return;
    }

    _sushi = start_sushi(options);

    if (_sushi == nullptr)
    {
        [NSApp terminate:nil];
        return;
    }

    _statusBar = [[SushiStatusBar alloc] initWithController:_sushi->controller()];
    _restrictAppleQuit = YES;

    __weak SushiAppDelegate* weakSelf = self;
    _keyDownMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                            handler:^NSEvent* (NSEvent* event)
    {
        auto modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        NSString* key = event.charactersIgnoringModifiers.lowercaseString;
        if ((modifiers & NSEventModifierFlagCommand) && [key isEqualToString:@"q"])
        {
            [weakSelf showQuitViaStatusBarPanel];
            return nil;
        }
        return event;
    }];
}

- (void)requestAuthorizedTermination:(id)sender
{
    _allowTermination = YES;
    [NSApp terminate:sender];
}

- (void)showQuitViaStatusBarPanel
{
    if (_showingQuitAlert)
    {
        [_quitWarningPanel makeKeyAndOrderFront:nil];
        return;
    }

    if (_quitWarningPanel == nil)
    {
        _quitWarningPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 170)
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [_quitWarningPanel setTitle:@"Quit Sushi from the status bar menu"];
        [_quitWarningPanel setReleasedWhenClosed:NO];
        [_quitWarningPanel setFloatingPanel:YES];
        [_quitWarningPanel setLevel:NSFloatingWindowLevel];
        _quitWarningPanel.delegate = self;

        NSView* contentView = _quitWarningPanel.contentView;

        auto* bodyLine1 = [NSTextField labelWithString:@"Apple Quit and Command-Q are disabled for Sushi GUI."];
        bodyLine1.frame = NSMakeRect(20, 102, 480, 22);
        bodyLine1.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
        [contentView addSubview:bodyLine1];

        auto* bodyLine2 = [NSTextField labelWithString:@"Quit from the status bar menu, or shut it down remotely via gRPC."];
        bodyLine2.frame = NSMakeRect(20, 72, 480, 18);
        [contentView addSubview:bodyLine2];

        auto* bodyLine3 = [NSTextField labelWithString:@"The app will stay running until one of those paths is used."];
        bodyLine3.frame = NSMakeRect(20, 50, 480, 18);
        [contentView addSubview:bodyLine3];

        auto* button = [[NSButton alloc] initWithFrame:NSMakeRect(220, 12, 80, 30)];
        button.title = @"OK";
        button.bezelStyle = NSBezelStyleRounded;
        button.keyEquivalent = @"\r";
        button.keyEquivalentModifierMask = 0;
        button.target = self;
        button.action = @selector(closeQuitViaStatusBarPanel:);
        [contentView addSubview:button];
        [_quitWarningPanel setDefaultButtonCell:button.cell];
    }

    _showingQuitAlert = YES;
    [_quitWarningPanel center];
    [NSApp activateIgnoringOtherApps:YES];
    [_quitWarningPanel makeKeyAndOrderFront:nil];
}

- (void)closeQuitViaStatusBarPanel:(id)sender
{
    (void)sender;
    [_quitWarningPanel close];
}

- (void)windowWillClose:(NSNotification*)notification
{
    if (notification.object == _quitWarningPanel)
    {
        _showingQuitAlert = NO;
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    (void)sender;

    if (_restrictAppleQuit && !_allowTermination)
    {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self showQuitViaStatusBarPanel];
        });
        return NSTerminateCancel;
    }

    if (_statusBar)
    {
        [_statusBar teardown];
        _statusBar = nil;
    }

    if (_keyDownMonitor != nil)
    {
        [NSEvent removeMonitor:_keyDownMonitor];
        _keyDownMonitor = nil;
    }

    if (_sushi)
    {
        _sushi->stop();
        _sushi.reset();
        ELKLOG_LOG_INFO("Sushi exiting normally!");
    }

    _allowTermination = NO;
    return NSTerminateNow;
}

@end

int main(int argc, char* argv[])
{
    signal(SIGINT, exit_on_signal);
    signal(SIGTERM, exit_on_signal);
    signal(SIGPIPE, pipe_signal_handler);

    @autoreleasepool
    {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        SushiAppDelegate* delegate = [[SushiAppDelegate alloc] initWithArgc:argc argv:argv];
        [app setDelegate:delegate];

        [app run];
    }

    return 0;
}
