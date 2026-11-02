NR's menus in your language
===========================

NR Control (the OptiScaler route) and the DLSSNR-AMD page in ReShade's Add-ons tab take their texts from a file
in this folder. English.txt holds every text in English.

Making a language
1. Copy English.txt in this folder and name the copy after your language, for example Deutsch.txt.
   Do not translate English.txt itself: every install replaces it.
2. Open the copy in a text editor that saves UTF-8 (Notepad does).
3. Change the line under msgid "@language" to your language's name as the menu should show it:
       msgid "@language"
       msgstr "Deutsch"
4. Translate the text in quotes after each msgstr. Leave the msgid lines as they are: NR finds each text by them.
   - Keep %d, %u, %s, %.2f and the like, in the same order: NR puts numbers and names there. A translation whose
     %-values differ from the English text's is not used, and the English text is shown.
   - \n is a new line, \" a quotation mark, \\ a backslash.
   - A text left in English, or removed from the file, is shown in English.
5. Choose the language: in NR Control, on its NR Control page; on the ReShade route, on the Language page of the
   DLSSNR-AMD add-on. The file is read again each time you save it, so the translation can be checked in the game.

Messages that NR writes as it runs (why a layer waits, what HUD FIX does now, the engine data) are in
English.txt too, after the menus' texts: some are made of several pieces with values between them - a
message is shown translated when the pieces cover most of it, else in English. The event list and the bug
report stay in English.

Letters
NR Control's font has Latin, Greek and Cyrillic letters. For other letters (Chinese, Japanese, Korean, ...) put a
font file that has them into this folder and give its name under msgid "@font", for example:
       msgid "@font"
       msgstr "NotoSansSC-Regular.ttf"
A full path works too. Use a .ttf or .ttc file (most .otf files cannot be read). A variable font (VF or [wght] in
its name) shows its built-in default weight, often the thinnest: use a static Regular file.

On the ReShade route the texts use ReShade's own font, not the @font above. ReShade follows the system's language
and takes that language's Windows font (Chinese: C:\Windows\Fonts\simsun.ttc); otherwise choose a font that has
your letters in ReShade's own settings (Font).

When the font in use lacks letters of the language chosen, the menu shows English and says why.
Text size (NR Control's NR Control page; the Language page on the ReShade route) makes the menu's text larger or
smaller.

Updates and uninstalling
New versions add texts: they show in English until your file has them (compare it with the new English.txt).
Uninstalling removes this folder: keep a copy of your file somewhere else.
