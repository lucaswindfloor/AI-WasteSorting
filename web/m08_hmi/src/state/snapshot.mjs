const nonNegative = /^(0|[1-9][0-9]*)$/u;

export class SnapshotState {
  snapshot = null;
  online = false;

  apply(snapshot) {
    if (typeof snapshot?.revision !== 'string' || !nonNegative.test(snapshot.revision) ||
      typeof snapshot.epoch !== 'string' || !nonNegative.test(snapshot.epoch)) {
      throw new Error('Invalid runtime revision');
    }
    const prior = this.snapshot;
    if (prior && snapshot.station === prior.station && snapshot.run === prior.run &&
        snapshot.session === prior.session &&
        (BigInt(snapshot.revision) < BigInt(prior.revision) || BigInt(snapshot.epoch) < BigInt(prior.epoch))) {
      throw new Error('Out-of-order runtime revision');
    }
    this.snapshot = snapshot;
    this.online = true;
  }

  disconnect() {
    this.online = false;
  }

  clear() {
    this.snapshot = null;
    this.online = false;
  }
}
