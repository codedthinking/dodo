* KNOWN-BAD (command classification): 'scalar list' is classified as a
* transformation, so dodoc drops its result SQL and emits NOTHING for it.
* Phase 4 (command registry) gives commands proper kinds; this case should
* then emit the VALUES table of scalars. Expected output is currently empty.
scalar x = 5
scalar list
