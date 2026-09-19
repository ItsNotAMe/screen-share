import type { WireObject } from './protocol';

export type SignalPeer = { peerId: string; role: string; generation: number };
export type SignalSession = {
  connectionId: string; hostGeneration: number; viewerGeneration: number;
  answered: boolean; hostCandidates: number; viewerCandidates: number;
  used: string[]; offers: number[];
};
export type SignalDecision = { ok: true; session: SignalSession } | { ok: false; reason: string };

// A fresh wire connectionId identifies every offer/answer/ICE generation,
// including ICE restart. The media PeerConnection itself may be reused.
export function authorizeSignal(message: WireObject, sender: SignalPeer, target: SignalPeer,
  current: SignalSession | undefined, digest: string, now: number): SignalDecision {
  const reject = (reason: string): SignalDecision => ({ ok: false, reason });
  if (sender.peerId === target.peerId || sender.role === target.role) return reject('forbidden');
  const host = sender.role === 'host' ? sender : target;
  const viewer = sender.role === 'viewer' ? sender : target;
  const id = message.connectionId as string;
  if (message.type === 'signal.offer') {
    if (sender.role !== 'host') return reject('forbidden');
    const used = current?.used ?? [];
    const offers = (current?.offers ?? []).filter(time => time > now - 60000);
    if (used.includes(digest)) return reject('stale_connection');
    // Initial offer plus three recovery offers per rolling minute. History is
    // bounded without forgetting IDs and accidentally permitting stale reuse.
    if (used.length >= 1024 || offers.length >= 4) return reject('rate_limited');
    return { ok: true, session: { connectionId: id, hostGeneration: host.generation, viewerGeneration: viewer.generation,
      answered: false, hostCandidates: 0, viewerCandidates: 0, used: [...used, digest], offers: [...offers, now] } };
  }
  if (!current || current.connectionId !== id || current.hostGeneration !== host.generation || current.viewerGeneration !== viewer.generation)
    return reject('stale_connection');
  const session = structuredClone(current);
  if (message.type === 'signal.answer') {
    if (sender.role !== 'viewer' || session.answered) return reject('forbidden');
    session.answered = true;
  } else if (message.type === 'signal.restart_request') {
    if (sender.role !== 'viewer') return reject('forbidden');
  } else if (message.type === 'signal.candidate') {
    // A viewer must send its answer before trickling its local candidates.
    if (sender.role === 'viewer' && !session.answered) return reject('invalid_state');
    const key = sender.role === 'host' ? 'hostCandidates' : 'viewerCandidates';
    if (++session[key] > 64) return reject('candidate_limit');
  } else return reject('invalid_command');
  return { ok: true, session };
}
