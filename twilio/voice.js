// kid_phone: SIP Domain voice URL. Dials the E.164 number in the SIP To user
// part, only if it is on the ALLOWED list (blocks toll fraud).
// TEST_MODE=echo answers the device directly: say, record 8 s, play back.
//
// The <Dial> posts back here when it ends (DialCallStatus set). If nobody
// picked up, the phone's leg is still open, so the child hears the asset
// /no-answer.wav (the phone's own voice, twilio/no_answer.wav, uploaded by
// provisioning in the same build as this function) before Twilio hangs up.
//
// Language: the phone names its prompt language in the INVITE header
// X-Kidphone-Lang ("en", "ca", "es"; APP_SIP_LANG_HEADER in
// src/app_config.h), which Twilio passes as event['SipHeader_X-Kidphone-Lang'].
// The Dial action URL carries it on as ?lang=, since the action request does
// not repeat the SIP headers. Missing or unknown = English.
const LANG_HEADER = 'X-Kidphone-Lang';
const LANGS = {
  en: {
    asset: '/no-answer.wav',
    say: 'en-US',
    noAnswer: "They didn't answer the phone.",
    cantCall: "Sorry, this phone can't call that number.",
  },
  ca: {
    asset: '/no-answer-ca.wav',
    say: 'ca-ES',
    noAnswer: 'No han agafat el telèfon.',
    cantCall: 'Ho sento, aquest telèfon no pot trucar a aquest número.',
  },
  es: {
    asset: '/no-answer-es.wav',
    say: 'es-ES',
    noAnswer: 'No han contestado al teléfono.',
    cantCall: 'Lo siento, este teléfono no puede llamar a ese número.',
  },
};
// Silence after the clip, so the phone sees the call last longer than
// APP_SHORT_CALL_MS (5 s, src/app_config.h) even when the callee declines at
// once (clip 2.1 s + 4 s). A shorter call reads as a Twilio block on the
// phone, which then says "The phone can't make calls right now".
const NO_ANSWER_PAUSE_S = 4;

// ?lang= on the action request, else the SIP header (looked up without
// regard to case), else English.
function langOf(event) {
  let code = event.lang;
  if (!code) {
    const want = ('SipHeader_' + LANG_HEADER).toLowerCase();
    const key = Object.keys(event).find((k) => k.toLowerCase() === want);
    code = key ? event[key] : '';
  }
  code = String(code || '').trim().toLowerCase();
  return Object.prototype.hasOwnProperty.call(LANGS, code) ? code : 'en';
}

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
  const lang = langOf(event);
  const L = LANGS[lang];
  const base = context.DOMAIN_NAME ? 'https://' + context.DOMAIN_NAME : '';
  if (event.DialCallStatus) {
    // The <Dial> below ended. completed / answered: a normal call that is
    // over. failed: Twilio could not place it (Geo permissions, trial
    // account, bad number): hang up at once, so the phone sees a short call
    // and says its own "can't make calls, ask a grown-up" sentence.
    // no-answer / busy (declined): tell the child. canceled: the phone hung
    // up while ringing; Twilio ignores this answer.
    const st = event.DialCallStatus;
    console.log('kid_phone dial ended', st, event.DialCallDuration || '', lang);
    if (st === 'no-answer' || st === 'busy' || st === 'canceled') {
      if (base) {
        twiml.play(base + L.asset);
      } else {
        twiml.say({ language: L.say }, L.noAnswer);
      }
      twiml.pause({ length: NO_ANSWER_PAUSE_S });
    }
    twiml.hangup();
    return callback(null, twiml);
  }
  const m = /^sip:(\+?\d+)@/.exec(event.To || '');
  const to = m ? m[1] : '';
  const allowed = (context.ALLOWED || '').split(',').map((s) => s.trim()).filter(Boolean);
  console.log('kid_phone call', event.From, '->', to, lang);
  if (!to || !allowed.includes(to)) {
    // Only reachable when the phone and Twilio disagree about the contacts
    // (or someone else uses the login). Kept short: the phone follows it
    // with its own "ask a grown-up" sentence.
    twiml.say({ language: L.say }, L.cantCall);
    twiml.hangup();
    return callback(null, twiml);
  }
  // No answerOnBridge: the SIP leg is answered at once and Twilio plays
  // ringback in that media stream. Early media then a 200 OK lost all
  // audio after answer behind home NAT (2026-10-10).
  const action = base + '/voice' + (lang === 'en' ? '' : '?lang=' + lang);
  const dial = twiml.dial({ callerId: context.CALLER_ID, timeout: 30, action: action, method: 'POST' });
  dial.number(to);
  return callback(null, twiml);
};
