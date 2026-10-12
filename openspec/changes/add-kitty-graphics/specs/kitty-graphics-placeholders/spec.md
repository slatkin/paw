# Spec Delta

## Purpose

Lets programs show kitty images through ordinary text cells (U+10EEEE placeholders), so images work through tmux and other programs that only pass text and colors.

## ADDED Requirements

### Requirement: Virtual placements
`a=p,U=1` (or `a=T,U=1`) SHALL create a virtual placement for image `i` with `c` columns and `r` rows. A virtual placement SHALL NOT draw anything by itself and SHALL NOT move the cursor. Deletion with `d` equal to `i`, `I`, `n`, `N`, `r`, or `R` SHALL delete virtual placements. Other `d` values SHALL NOT affect them.

#### Scenario: Create a virtual placement
- **WHEN** a program sends `a=p,U=1,i=42,c=2,r=2,q=2`
- **THEN** nothing new is visible and the cursor does not move

#### Scenario: Delete all visible does not touch virtual placements
- **WHEN** image 42 has a virtual placement and a program sends `a=d,d=a`
- **THEN** placeholder cells for image 42 still show the image

### Requirement: Placeholder cells show image parts
A cell whose base character is U+10EEEE SHALL show part of an image instead of a glyph. The foreground color SHALL give the image id: the 8-bit index in 256-color mode, or the 24-bit RGB value in true color mode. A third diacritic SHALL give the most significant byte of the id. The underline color, when set, SHALL give the placement id. The first and second diacritics SHALL give the row and column, using the kitty `rowcolumn-diacritics` table.

#### Scenario: 2x2 placeholder
- **WHEN** image 42 has a virtual placement with `c=2,r=2`, and a program prints `ESC[38;5;42m` and then the four placeholder cells with diacritics (0,0) (0,1) on one line and (1,0) (1,1) on the next line
- **THEN** the four cells show the four quarters of image 42

#### Scenario: High byte of the id
- **WHEN** a program prints placeholders with foreground `38;5;42` and a third diacritic for value 2
- **THEN** the cells show image `42 + (2 << 24)`

#### Scenario: No matching image
- **WHEN** a placeholder cell refers to an image id that does not exist
- **THEN** the cell shows no glyph and no image, only its background

### Requirement: Inherited row and column
When a placeholder cell has missing diacritics, paw SHALL take the missing values from the placeholder cell to its left, by the three inheritance rules of the kitty spec. With no diacritics, the row and high byte SHALL come from the left cell and the column SHALL be the left column plus one. This SHALL apply only when the left cell has the same foreground and underline colors.

#### Scenario: Only the first column has diacritics
- **WHEN** a program prints, on each of two lines, one placeholder with a row diacritic followed by two placeholders with no diacritics, all with the same foreground color
- **THEN** the six cells show a 2-row by 3-column image

### Requirement: Image fit inside the placeholder area
paw SHALL scale the image to fit the `c` by `r` cell area of the virtual placement, keep its aspect ratio, and center it. Each placeholder cell SHALL show the matching part of that fitted image. Transparent pixels SHALL show the cell's background color.

#### Scenario: Wide image in a square area
- **WHEN** a 200x100 image has a virtual placement of 4x4 cells with square cells
- **THEN** the image fills the full width and is centered vertically, and the cells above and below it show their background

### Requirement: Placeholders behave as text
Placeholder cells SHALL scroll, reflow, enter the scrollback, be erased, and be selected and copied like other text cells. The image part SHALL move with its cell.

#### Scenario: Inside tmux
- **WHEN** a program inside tmux transmits an image with passthrough and tmux redraws its pane with placeholder cells
- **THEN** the image appears in the tmux pane and moves when tmux scrolls the pane

#### Scenario: Erase placeholder cells
- **WHEN** a program erases a line that holds placeholder cells
- **THEN** that part of the image disappears
