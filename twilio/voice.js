// kid_phone: SIP Domain voice URL. Dials the E.164 number in the SIP To user
// part, only if it is on the ALLOWED list (blocks toll fraud).
// TEST_MODE=echo answers the device directly: say, record 8 s, play back.
//
// The <Dial> posts back here when it ends (DialCallStatus set). If nobody
// picked up, the phone's leg is still open, so the child hears the asset
// /no-answer.wav (the phone's own voice, twilio/no_answer.wav, uploaded by
// provisioning in the same build as this function) before Twilio hangs up.
const NO_ANSWER_ASSET = '/no-answer.wav';
// Silence after the clip, so the phone sees the call last longer than
// APP_SHORT_CALL_MS (5 s, src/app_config.h) even when the callee declines at
// once (clip 2.1 s + 4 s). A shorter call reads as a Twilio block on the
// phone, which then says "The phone can't make calls right now".
const NO_ANSWER_PAUSE_S = 4;

exports.handler = function (context, event, callback) {
  const twiml = new Twilio.twiml.VoiceResponse();
  if (context.TEST_MODE === 'echo') {
    if (event.RecordingUrl) {
      twiml.say('Here is what I heard.');
      twiml.play(event.RecordingUrl);
      twiml.say('End of test. Goodbye.');
      twiml.hangup();
    } else {
      twiml.say('Echo test. This is Twilio talking to the handset. Speak after the beep.');
      twiml.record({ maxLength: 8, playBeep: true, timeout: 0, trim: 'do-not-trim' });
    }
    return callback(null, twiml);
  }
  const base = context.DOMAIN_NAME ? 'https://' + context.DOMAIN_NAME : '';
  if (event.DialCallStatus) {
    // The <Dial> below ended. completed / answered: a normal call that is
    // over. failed: Twilio could not place it (Geo permissions, trial
    // account, bad number): hang up at once, so the phone sees a short call
    // and says its own "can't make calls, ask a grown-up" sentence.
    // no-answer / busy (declined): tell the child. canceled: the phone hung
    // up while ringing; Twilio ignores this answer.
    const st = event.DialCallStatus;
    console.log('kid_phone dial ended', st, event.DialCallDuration || '');
    if (st === 'no-answer' || st === 'busy' || st === 'canceled') {
      if (base) {
        twiml.play(base + NO_ANSWER_ASSET);
      } else {
        twiml.say("They didn't answer the phone.");
      }
      twiml.pause({ length: NO_ANSWER_PAUSE_S });
    }
    twiml.hangup();
    return callback(null, twiml);
  }
  const m = /^sip:(\+?\d+)@/.exec(event.To || '');
  const to = m ? m[1] : '';
  const allowed = (context.ALLOWED || '').split(',').map((s) => s.trim()).filter(Boolean);
  console.log('kid_phone call', event.From, '->', to);
  if (!to || !allowed.includes(to)) {
    // Only reachable when the phone and Twilio disagree about the contacts
    // (or someone else uses the login). Kept short: the phone follows it
    // with its own "ask a grown-up" sentence.
    twiml.say("Sorry, this phone can't call that number.");
    twiml.hangup();
    return callback(null, twiml);
  }
  // No answerOnBridge: the SIP leg is answered at once and Twilio plays
  // ringback in that media stream. Early media then a 200 OK lost all
  // audio after answer behind home NAT (2026-10-10).
  const dial = twiml.dial({ callerId: context.CALLER_ID, timeout: 30, action: base + '/voice', method: 'POST' });
  dial.number(to);
  return callback(null, twiml);
};
