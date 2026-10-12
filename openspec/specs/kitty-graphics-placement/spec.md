# kitty-graphics-placement Specification

## Purpose
Defines how stored kitty images appear on the terminal screen: their position, size, stacking order relative to text, cursor movement, deletion, and how they follow scrolling, clearing, screen switches, reset, and resize.

## Requirements

### Requirement: Place at the cursor
`a=T` and `a=p` SHALL create a placement with its top-left corner at the top-left of the cursor cell, moved by `X` and `Y` pixels. `X` and `Y` SHALL be smaller than the cell size. A placement with a non-zero image id and placement id SHALL replace the existing placement with the same pair.

#### Scenario: Put at cursor
- **WHEN** the cursor is at row 3, column 5, and a program sends `a=p,i=1`
- **THEN** image 1 appears with its top-left at the top-left of that cell

#### Scenario: Move a placement
- **WHEN** placement `i=1,p=2` is on screen and a program sends `a=p,i=1,p=2` with the cursor in another cell
- **THEN** only one placement `i=1,p=2` exists, at the new cell

### Requirement: Source rectangle and display size
paw SHALL show the intersection of the source rectangle `x,y,w,h` with the image (the full image by default). With `c` and `r`, paw SHALL scale the shown area to fill that many columns and rows without distortion (letterbox or pillarbox). With only one of `c` or `r`, paw SHALL compute the other from the aspect ratio. Without both, the image keeps its pixel size.

#### Scenario: Cell size given
- **WHEN** a program places a 100x50 image with `c=10,r=5`
- **THEN** the image fills a 10x5 cell area, scaled to keep its aspect ratio

#### Scenario: Source rectangle
- **WHEN** a program places a 100x100 image with `x=50,y=0,w=50,h=100`
- **THEN** only the right half of the image appears

#### Scenario: Image wider than the screen
- **WHEN** an image placement extends past the right edge of the window
- **THEN** paw cuts the image at the right edge

### Requirement: Cursor movement after placement
After `a=T` or `a=p` with `C=0` (the default), paw SHALL move the cursor right by the number of columns and down by the number of rows that the placement covers. When the move goes past the bottom of the scroll region, paw SHALL scroll the text and the images. With `C=1`, paw SHALL NOT move the cursor.

#### Scenario: Default cursor movement
- **WHEN** the cursor is at row 0, column 0 and a program places an image that covers 4 columns and 3 rows
- **THEN** the cursor moves to the cell after the image's columns, on the image's last row

#### Scenario: No cursor movement
- **WHEN** a program places an image with `C=1`
- **THEN** the cursor position does not change

### Requirement: Z-index layers
A placement with `z >= 0` SHALL be drawn over the text. A placement with `-1073741824 <= z < 0` SHALL be drawn under the text and over all cell backgrounds. A placement with `z < -1073741824` SHALL be drawn under the text and SHALL show only in cells with the default background color. Overlapping placements SHALL be drawn in z order, then in image id order. Alpha SHALL blend.

#### Scenario: Image over text
- **WHEN** an opaque image with `z=0` covers cells that contain text
- **THEN** the text is hidden under the image

#### Scenario: Image under text
- **WHEN** an image with `z=-1` covers cells that contain text
- **THEN** the text glyphs are visible on top of the image

#### Scenario: Image under colored backgrounds
- **WHEN** an image with `z=-1073741825` covers a cell with a red background and a cell with the default background
- **THEN** the red cell shows red, and the default cell shows the image

### Requirement: Deletion
`a=d` SHALL delete placements as the `d` key selects: `a` (default; all visible), `i` (image id, and placement id when `p` is given), `n` (newest image with number `I`), `c` (at the cursor cell), `p` (at cell `x,y`, 1-based), `q` (at cell `x,y` with z-index `z`), `r` (image ids from `x` to `y`), `x` (in column `x`), `y` (in row `y`), `z` (with z-index `z`). Uppercase variants SHALL also delete the image data when no placement of the image is left.

#### Scenario: Delete all visible
- **WHEN** a program sends `a=d`
- **THEN** no placement is visible on screen, and the image data stays stored

#### Scenario: Delete and free
- **WHEN** image 4 has one placement and a program sends `a=d,d=I,i=4`
- **THEN** the placement disappears and image 4 no longer exists

#### Scenario: Delete at a cell
- **WHEN** two placements overlap the cell at column 3, row 4 and a program sends `a=d,d=p,x=3,y=4`
- **THEN** both placements disappear and other placements stay

#### Scenario: Animation frame deletion
- **WHEN** a program sends `a=d,d=f`
- **THEN** paw changes no state

### Requirement: Text edits do not change images
Writing text over a placement, erasing in line, erasing characters, and inserting or deleting characters SHALL NOT delete or move placements. Only the events in the other requirements of this capability SHALL change placements. Inserting or deleting lines scrolls part of the screen, so the scrolling requirement applies.

#### Scenario: Text over image
- **WHEN** a placement with `z=-1` is on screen and a program prints text over its cells
- **THEN** the placement stays, with the text drawn on top

#### Scenario: Erase in line
- **WHEN** a program sends `CSI 2 K` on a row that a placement covers
- **THEN** the placement stays

### Requirement: Scrolling
Placements SHALL move with the text when the screen scrolls, into and through the scrollback, and SHALL be visible when the user scrolls back. A placement whose top row leaves the scrollback SHALL be deleted. In a scroll region, only placements fully inside the region SHALL move. Parts that move outside the region SHALL be clipped.

#### Scenario: Scroll into history
- **WHEN** an image is on screen and the program prints enough lines to scroll it off the top
- **THEN** the image is not visible, and when the user scrolls back, the image is visible next to the same text

#### Scenario: Scroll region
- **WHEN** a scroll region covers rows 5 to 15, one placement is fully inside it and one is above it, and the region scrolls up one line
- **THEN** the inside placement moves up one row, and the placement above the region does not move

#### Scenario: Delete line
- **WHEN** a placement is below the cursor row and a program sends `CSI M` (delete line)
- **THEN** the placement moves up one row with the text below the cursor

### Requirement: Clear, alternate screen, and reset
Clear screen (`CSI 2 J` and `CSI 3 J`) SHALL delete all placements that are visible on screen. Entering the alternate screen SHALL delete all placements and images of the alternate screen. Main and alternate screens SHALL each have their own images and placements. A terminal reset (RIS) SHALL delete all placements and images.

#### Scenario: Clear command
- **WHEN** an image is on screen and the user runs `clear`
- **THEN** no image is visible

#### Scenario: Alternate screen
- **WHEN** an image is on the main screen and a program enters the alternate screen, places an image, and exits the alternate screen
- **THEN** the main screen shows its original image, and the alternate screen image is gone

### Requirement: Resize and font size
When the window width changes and the text reflows, paw SHALL move each placement by the same number of lines as the text near the cursor moves. When the cell size changes (font size change), paw SHALL scale each placement so that it covers the same number of columns and rows as before.

#### Scenario: Font zoom
- **WHEN** an image covers 10 columns and 5 rows and the user increases the font size
- **THEN** the image still covers 10 columns and 5 rows, at a larger pixel size

#### Scenario: Window narrower
- **WHEN** an image is under a line of output and the window is made narrower so that earlier lines wrap
- **THEN** the image is still visible and not deleted
