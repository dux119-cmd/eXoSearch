# eXoSearch
A fast CLI search &amp; launch tool for eXoDOS and eXoWin31

# Build
`cmake -B build && cmake --build build`

# Launch
`build/eds /path/to/MS-DOS.xml`

# Operation
- **Type** words to search for a game.
- **Tab** expands the current word if there's only one pattern of word matches remaining.
- **Page Up/Down** and **Up/Down Arrow** steps through the list or the available search matches.
- **Enter** launches the the sole remaining hit or the selected game.

# Tips
Prefer the most unique parts of the game's title. For example,
`king yonder` isolates *"King's Quest V: Absence Makes the Heart Go Yonder!"*
because not many DOS games have 'yonder' in their title.

You can add the year, developer, or publisher dates or names to also narrow your search.
For example, if you remember playing a tank game from the 80s authored by Spectrum Holobyte,
try: `tank 198 holo` isolates *"Tank: The M1A1 Abrams Battle Tank Simulation 1989 Sphere, Inc. Spectrum Holobyte"*
