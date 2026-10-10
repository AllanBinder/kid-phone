// kid_phone: SIP Domain voice URL. Dials the E.164 number in the SIP To user
// part, only if it is on the ALLOWED list (blocks toll fraud).
// TEST_MODE=echo answers the device directly: say, record 8 s, play back.
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
  const m = /^sip:(\+?\d+)@/.exec(event.To || '');
  const to = m ? m[1] : '';
  const allowed = (context.ALLOWED || '').split(',').map((s) => s.trim()).filter(Boolean);
  console.log('kid_phone call', event.From, '->', to);
  if (!to || !allowed.includes(to)) {
    twiml.say('This number is not allowed.');
    twiml.hangup();
    return callback(null, twiml);
  }
  // No answerOnBridge: the SIP leg is answered at once and Twilio plays
  // ringback in that media stream. Early media then a 200 OK lost all
  // audio after answer behind home NAT (2026-10-10).
  const dial = twiml.dial({ callerId: context.CALLER_ID, timeout: 30 });
  dial.number(to);
  return callback(null, twiml);
};
