// State callbacks must perform only local work. Network delivery deliberately
// runs outside their serializer and outside the Durable Object input gate.
export class BackgroundOutbox<Job> {
  private requested = false;
  private running = false;
  private keepAlive: (work: Promise<void>) => void;
  private next: () => Promise<Job | undefined>;
  private deliver: (job: Job) => Promise<boolean>;
  constructor(keepAlive: (work: Promise<void>) => void,
    next: () => Promise<Job | undefined>, deliver: (job: Job) => Promise<boolean>) {
    this.keepAlive = keepAlive; this.next = next; this.deliver = deliver;
  }
  request(): void { this.requested = true; }
  start(): void {
    if (this.running || !this.requested) return;
    this.running = true;
    this.requested = false;
    this.keepAlive(this.run().catch(() => false).then(success => {
      this.running = false;
      // A failed attempt is retried by the persisted alarm or a later mutation,
      // not an unbounded immediate retry loop. At most one delivery is active.
      if (!success) this.requested = false;
      else if (this.requested) this.start();
    }));
  }
  private async run(): Promise<boolean> {
    for (let i = 0; i < 4; ++i) {
      const job = await this.next();
      if (!job) return true;
      if (!await this.deliver(job)) return false;
    }
    // A continuously changing room coalesces into one persisted latest value.
    // After four jobs the alarm provides another bounded opportunity to drain.
    this.requested = false;
    return true;
  }
}

export class SerializedState {
  private tail: Promise<void> = Promise.resolve();
  run<T>(action: () => Promise<T>): Promise<T> {
    const work = this.tail.then(action);
    this.tail = work.then(() => {}, () => {});
    return work;
  }
}
