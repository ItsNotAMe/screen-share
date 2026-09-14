// Pure subscription ordering policy, with no transport or automatic HTTP polls.
export class RevisionTracker {
  private generation = 0;
  private active = false;
  private pending = false;
  private acceptedRevision: number | null = null;
  get revision(): number | null { return this.acceptedRevision; }
  private advance(): void {
    if (this.generation === Number.MAX_SAFE_INTEGER) throw new Error("Subscription generation exhausted");
    ++this.generation;
  }
  start(): number { this.advance(); this.active = true; this.acceptedRevision = null; this.pending = false; return this.generation; }
  stop(): void { this.advance(); this.active = false; this.acceptedRevision = null; this.pending = false; }
  private current(generation: number, revision: number): boolean {
    return this.active && generation === this.generation && Number.isSafeInteger(revision) && revision >= 0;
  }
  snapshot(generation: number, revision: number): "ignore" | "apply" {
    if (!this.current(generation, revision) || (this.revision !== null && revision < this.revision)) return "ignore";
    this.acceptedRevision = revision; this.pending = false; return "apply";
  }
  delta(generation: number, revision: number): "ignore" | "apply" | "resync" {
    if (!this.current(generation, revision) || this.pending || (this.revision !== null && revision <= this.revision)) return "ignore";
    if (this.revision === null || revision !== this.revision + 1) { this.pending = true; return "resync"; }
    this.acceptedRevision = revision; return "apply";
  }
}
