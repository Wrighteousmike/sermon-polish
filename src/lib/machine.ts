// How capable this computer is for audio processing. Cleaning a sermon is heavy work that uses every
// processor core, so a computer with only a few cores is much slower (and playback can stutter while it works).
const fake = import.meta.env.DEV ? Number(new URLSearchParams(window.location.search).get('cores') ?? 0) : 0

/** Logical processors (threads). 0 if the system doesn't say. */
export const CORES = fake > 0 ? fake : typeof navigator !== 'undefined' ? navigator.hardwareConcurrency || 0 : 0

/** A computer on the slower side for this: 4 or fewer logical processors. */
export const SLOW_MACHINE = CORES > 0 && CORES <= 4
