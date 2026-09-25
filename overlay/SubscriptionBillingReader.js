(() => {
  // Only return a date and renewal state. Never return page text, identity,
  // invoices, payment methods, cookies or tokens to the native application.
  const months = {
    january: 1, february: 2, march: 3, april: 4, may: 5, june: 6,
    july: 7, august: 8, september: 9, october: 10, november: 11, december: 12,
    jan: 1, feb: 2, mar: 3, apr: 4, jun: 6, jul: 7, aug: 8, sep: 9, sept: 9, oct: 10, nov: 11, dec: 12
  };
  const cue = /(续订|續訂|续费|續費|有效至|有效期|直至|到期|renew|bill(?:ing)?\s+date|valid\s+(?:until|through)|expire|until|through|次回請求|自動更新|有効期限|まで利用|결제일|자동\s*갱신|만료|까지)/i;
  const cancelled = /(已取消|不再续订|不再續訂|不再续费|不再續費|不再自动续|不再自動續|cancell?ed|will\s+not\s+renew|won't\s+renew|キャンセル|更新されません|취소|갱신되지\s*않)/i;
  const renewing = /(自动续订|自動續訂|自动续费|自動續費|下次续费|下次續費|下次扣费|下次扣費|renew(?:s|al)?\s+automatically|automatically\s+renew|next\s+billing\s+date|next\s+payment|次回請求|自動更新|다음\s*결제|자동\s*갱신)/i;
  const datePatterns = [
    /\b(20\d{2})\s*(?:年|년|[.\/-])\s*(\d{1,2})\s*(?:月|월|[.\/-])\s*(\d{1,2})\s*(?:日|일)?\b/g,
    /\b([A-Za-z]+)\s+(\d{1,2}),?\s+(20\d{2})\b/g,
    /\b(\d{1,2})\s+([A-Za-z]+)\s+(20\d{2})\b/g
  ];
  function datesIn(text) {
    const found = [];
    for (let i = 0; i < datePatterns.length; i++) {
      const re = new RegExp(datePatterns[i].source, 'g');
      let match;
      while ((match = re.exec(text))) {
        let y, m, d;
        if (i === 0) [y, m, d] = match.slice(1, 4).map(Number);
        else if (i === 1) { m = months[match[1].toLowerCase()]; d = Number(match[2]); y = Number(match[3]); }
        else { d = Number(match[1]); m = months[match[2].toLowerCase()]; y = Number(match[3]); }
        if (m && m <= 12 && d >= 1 && d <= 31) found.push(`${y}-${String(m).padStart(2, '0')}-${String(d).padStart(2, '0')}`);
      }
    }
    return [...new Set(found)];
  }
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  const matches = [];
  let node;
  while ((node = walker.nextNode())) {
    const parent = node.parentElement;
    if (!parent || parent.closest('script,style,textarea,input,table,nav,[hidden],[aria-hidden="true"]')) continue;
    if (!parent.getClientRects().length) continue;
    const text = (node.textContent || '').trim();
    if (text.length < 8 || text.length > 700 || !cue.test(text)) continue;
    const dates = datesIn(text);
    if (dates.length > 1) return null;
    if (dates.length !== 1) continue;
    const renewal = cancelled.test(text) ? 'cancelled' : renewing.test(text) ? 'renewing' : 'unknown';
    matches.push({ date: dates[0], renewal });
  }
  const unique = [...new Map(matches.map(x => [JSON.stringify(x), x])).values()];
  return unique.length === 1 ? unique[0] : null;
})()
