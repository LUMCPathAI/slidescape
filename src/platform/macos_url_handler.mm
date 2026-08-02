/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#import <Cocoa/Cocoa.h>
#include "SDL_syswm.h"
#include <stdlib.h>
#include <string.h>

static NSMutableArray* macos_pending_urls;
static id macos_url_handler;
@class SlidescapeLiveResizeHandler;
static SlidescapeLiveResizeHandler* macos_live_resize_handler;

typedef void (*macos_live_resize_callback_t)(void* userdata);

@interface SlidescapeURLHandler : NSObject
- (void)handleGetURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent;
@end

@interface SlidescapeLiveResizeHandler : NSObject {
	NSWindow* window;
	NSTimer* timer;
	macos_live_resize_callback_t callback;
	void* callback_userdata;
}
- (id)initWithWindow:(NSWindow*)source_window
            callback:(macos_live_resize_callback_t)source_callback
            userdata:(void*)source_userdata;
- (void)stopLiveResizeRedraw;
@end

@implementation SlidescapeLiveResizeHandler
- (id)initWithWindow:(NSWindow*)source_window
            callback:(macos_live_resize_callback_t)source_callback
            userdata:(void*)source_userdata {
	self = [super init];
	if (self) {
		window = source_window;
		callback = source_callback;
		callback_userdata = source_userdata;
		NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
		[center addObserver:self selector:@selector(windowWillStartLiveResize:)
		               name:NSWindowWillStartLiveResizeNotification object:window];
		[center addObserver:self selector:@selector(windowDidEndLiveResize:)
		               name:NSWindowDidEndLiveResizeNotification object:window];
	}
	return self;
}

- (void)windowWillStartLiveResize:(NSNotification*)notification {
	(void)notification;
	NSInteger refresh_rate = 60;
	NSScreen* screen = [window screen];
	if (@available(macOS 12.0, *)) {
		refresh_rate = [screen maximumFramesPerSecond];
	}
	if (refresh_rate <= 0) {
		refresh_rate = 60;
	}

	[timer invalidate];
	timer = [NSTimer timerWithTimeInterval:1.0 / (double)refresh_rate
	                               target:self
	                             selector:@selector(redrawDuringLiveResize:)
	                             userInfo:nil
	                              repeats:YES];
	[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
	callback(callback_userdata);
}

- (void)redrawDuringLiveResize:(NSTimer*)source_timer {
	(void)source_timer;
	callback(callback_userdata);
}

- (void)windowDidEndLiveResize:(NSNotification*)notification {
	(void)notification;
	[timer invalidate];
	timer = nil;
	callback(callback_userdata);
}

- (void)stopLiveResizeRedraw {
	[timer invalidate];
	timer = nil;
	[[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)dealloc {
	[self stopLiveResizeRedraw];
#if !__has_feature(objc_arc)
	[super dealloc];
#endif
}
@end

@implementation SlidescapeURLHandler
- (void)handleGetURLEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent {
	(void)replyEvent;
	NSString* url = [[event paramDescriptorForKeyword:keyDirectObject] stringValue];
	if (url) {
		@synchronized([SlidescapeURLHandler class]) {
			if (!macos_pending_urls) {
				macos_pending_urls = [[NSMutableArray alloc] init];
			}
			[macos_pending_urls addObject:url];
		}
	}
}
@end

extern "C" void macos_register_url_handler(void) {
	@synchronized([SlidescapeURLHandler class]) {
		if (!macos_url_handler) {
			macos_url_handler = [[SlidescapeURLHandler alloc] init];
			if (!macos_pending_urls) {
				macos_pending_urls = [[NSMutableArray alloc] init];
			}
			[[NSAppleEventManager sharedAppleEventManager] setEventHandler:macos_url_handler
			                                                   andSelector:@selector(handleGetURLEvent:withReplyEvent:)
			                                                 forEventClass:kInternetEventClass
			                                                    andEventID:kAEGetURL];
		}
	}
}

extern "C" char* macos_copy_next_open_url(void) {
	char* result = NULL;
	@synchronized([SlidescapeURLHandler class]) {
		if ([macos_pending_urls count] > 0) {
			NSString* url = [macos_pending_urls objectAtIndex:0];
			const char* utf8 = [url UTF8String];
			if (utf8) {
				size_t len = strlen(utf8);
				result = (char*)malloc(len + 1);
				if (result) {
					memcpy(result, utf8, len + 1);
				}
			}
			[macos_pending_urls removeObjectAtIndex:0];
		}
	}
	return result;
}

extern "C" bool macos_start_live_resize_redraw(SDL_Window* sdl_window,
		macos_live_resize_callback_t callback, void* userdata) {
	SDL_SysWMinfo window_info = {};
	SDL_VERSION(&window_info.version);
	if (!SDL_GetWindowWMInfo(sdl_window, &window_info) || !window_info.info.cocoa.window) {
		return false;
	}

	if (macos_live_resize_handler) {
		[macos_live_resize_handler stopLiveResizeRedraw];
#if !__has_feature(objc_arc)
		[macos_live_resize_handler release];
#endif
	}
	macos_live_resize_handler = [[SlidescapeLiveResizeHandler alloc]
		initWithWindow:window_info.info.cocoa.window callback:callback userdata:userdata];
	return macos_live_resize_handler != nil;
}

extern "C" void macos_stop_live_resize_redraw(void) {
	if (macos_live_resize_handler) {
		[macos_live_resize_handler stopLiveResizeRedraw];
#if !__has_feature(objc_arc)
		[macos_live_resize_handler release];
#endif
		macos_live_resize_handler = nil;
	}
}
