// macOS's own open and save panels, for `FileDialogs` (file_dialog.hpp).
//
// **Modal, run where they are asked for**, as Windows' dialogs are: `runModal` turns AppKit's
// event loop inside the click, which goes on drawing the window behind it, and the answer is in
// before `open` or `save` returns. Not the panels' completion-handler form: on macOS 26 a handler
// can run before the panel's modal session has finished unwinding, and a dialog opened from inside
// it hangs (Mozilla's bug 2053177) — and a handler would outlive a window closed while its
// panel was up, which this form cannot.

#include "ui/file_dialog.hpp"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace takt4::ui {

namespace {

NSString* textOf(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()]; // nil for bytes that are not UTF-8
}

/// What the panel lets the operator pick: a preset is JSON; a fixture's definition is GDTF or an
/// Open Fixture Library file, which is JSON too.
NSArray<UTType*>* typesFor(FileKind kind) {
    if (kind == FileKind::FixtureDefinition) {
        UTType* const gdtf = [UTType typeWithFilenameExtension:@"gdtf"];
        return gdtf != nil ? @[ gdtf, UTTypeJSON ] : @[ UTTypeJSON ];
    }
    return @[ UTTypeJSON ];
}

std::filesystem::path pathOf(NSURL* url) {
    return url != nil && url.fileURL ? std::filesystem::path(url.fileSystemRepresentation)
                                     : std::filesystem::path{};
}

} // namespace

struct FileDialogs::Pending {};

FileDialogs::FileDialogs() = default;
FileDialogs::~FileDialogs() = default;

bool FileDialogs::waiting() const {
    return false;
}

void FileDialogs::open(const std::string& title, FileKind kind, Chosen chosen) {
    FileChoice choice;
    if (fileDialogsAllowed()) {
        @autoreleasepool {
            NSOpenPanel* const panel = [NSOpenPanel openPanel];
            panel.message = textOf(title); // an open panel shows no title of its own
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.allowedContentTypes = typesFor(kind);
            if ([panel runModal] == NSModalResponseOK) {
                choice.path = pathOf(panel.URL);
            }
        }
    }
    chosen(choice);
}

void FileDialogs::save(const std::string& title, const std::string& suggested, Chosen chosen) {
    FileChoice choice;
    if (fileDialogsAllowed()) {
        @autoreleasepool {
            NSSavePanel* const panel = [NSSavePanel savePanel];
            panel.message = textOf(title);
            panel.nameFieldStringValue = textOf(suggested);
            panel.allowedContentTypes = @[ UTTypeJSON ];
            if ([panel runModal] == NSModalResponseOK) {
                choice.path = pathOf(panel.URL);
            }
        }
    }
    chosen(choice);
}

} // namespace takt4::ui
