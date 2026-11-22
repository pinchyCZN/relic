PCDEMOS RELIC                                              Analysis Note


                        WHERE THE JOURNEY GOES


   This directory is the only place in the project where the search
   may be written down. Everything in it except this file is
   gitignored and nothing ever reads it twice.

   IT EXISTS BECAUSE THE JOURNEY HAD NOWHERE TO GO. Rule 10 bans it
   from docs/ and rule 18 bans it from the code, so it leaked into
   whichever was being written at the time - and the leak moved from
   source comments into docs once the comment budget started biting.
   A note with a home does not leak.

   Put here:

     o what was tried that did not work, and why it looked plausible
     o a dead end worth not repeating
     o a hunch with no evidence yet
     o a half-read routine and where the reading stopped
     o anything beginning "I thought", "it turned out", "first I"

   Never put here anything that belongs in docs/ or in the Ghidra
   database. A FACT IN notes/ IS A FACT LOST - the checks do not read
   this directory, checkrefs does not resolve its links, and nobody
   greps it.

   So the test when writing a line is the same one rule 10 asks:
   does this describe the BINARY, or the history of our work on it?
   The binary goes to docs/. The history comes here, and may be
   deleted at any time without asking.
