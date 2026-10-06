/* =========================================================
   Lingua — Application Logic
   ========================================================= */
'use strict';

/* ---------- Data ---------- */
const LANG_META = {
  en: { name: '英语', flag: 'EN', native: 'English' },
  ja: { name: '日语', flag: 'JA', native: '日本語' },
  ko: { name: '韩语', flag: 'KO', native: '한국어' },
};

const COURSES = {
  en: [
    { code: 'A1', title: '入门 Starter', desc: '打招呼、数字、基础句型，约 200 词' },
    { code: 'A2', title: '初级 Elementary', desc: '日常对话、时态入门，约 600 词' },
    { code: 'B1', title: '中级 Intermediate', desc: '复杂句、观点表达，约 1500 词' },
    { code: 'B2', title: '中高级 Upper-Intermediate', desc: '流畅交流、抽象话题，约 3000 词' },
    { code: 'C1', title: '高级 Advanced', desc: '学术写作、地道表达，5000+ 词' },
  ],
  ja: [
    { code: 'A1', title: '入门 初級', desc: '五十音、寒暄、基本助词' },
    { code: 'A2', title: '初级 初中級', desc: '动词变形、日常会话' },
    { code: 'B1', title: '中级 中級', desc: '敬语入门、复杂表达' },
    { code: 'B2', title: '中高级', desc: '商务日语、读解' },
    { code: 'C1', title: '高级 上級', desc: '论文级写作、母语级表达' },
  ],
  ko: [
    { code: 'A1', title: '入门 입문', desc: '한글、寒暄、基本助词' },
    { code: 'A2', title: '初级 초급', desc: '活用形、日常对话' },
    { code: 'B1', title: '中级 중급', desc: '敬语、连接词尾' },
    { code: 'B2', title: '中高级 중상급', desc: '新闻、议论文' },
    { code: 'C1', title: '高级 고급', desc: '专业领域、母语级' },
  ],
};

const VOCAB = {
  en: [
    { w: 'serendipity', p: '/ˌserənˈdɪpəti/', m: '意外发现美好事物的能力', e: 'Meeting her was pure serendipity.' },
    { w: 'ephemeral', p: '/ɪˈfemərəl/', m: '短暂的，瞬息的', e: 'The beauty of cherry blossoms is ephemeral.' },
    { w: 'resilient', p: '/rɪˈzɪliənt/', m: '有韧性的，能恢复的', e: 'Children are remarkably resilient.' },
    { w: 'meticulous', p: '/məˈtɪkjələs/', m: '一丝不苟的', e: 'She is meticulous about her work.' },
    { w: 'ubiquitous', p: '/juːˈbɪkwɪtəs/', m: '无处不在的', e: 'Smartphones have become ubiquitous.' },
    { w: 'eloquent', p: '/ˈeləkwənt/', m: '雄辩的，有口才的', e: 'He gave an eloquent speech.' },
    { w: 'pragmatic', p: '/præɡˈmætɪk/', m: '务实的', e: 'We need a pragmatic approach.' },
    { w: 'nostalgia', p: '/nɒˈstældʒə/', m: '怀旧，乡愁', e: 'A wave of nostalgia washed over me.' },
  ],
  ja: [
    { w: '木漏れ日', p: 'こもれび', m: '从树叶缝隙洒下的阳光', e: '森の中で木漏れ日が心地よい。' },
    { w: '頑張る', p: 'がんばる', m: '努力，加油', e: '明日も頑張りましょう。' },
    { w: 'お世話になる', p: 'おせわになる', m: '承蒙关照', e: 'いつもお世話になっております。' },
    { w: '一期一会', p: 'いちごいちえ', m: '一生只此一次的相遇', e: '一期一会の気持ちで接する。' },
    { w: '詫びる', p: 'わびる', m: '道歉，谢罪', e: '心から詫びます。' },
    { w: '粋', p: 'いき', m: '潇洒，雅致', e: '彼の生き方は粋だ。' },
  ],
  ko: [
    { w: '정', p: 'jeong', m: '情，情义', e: '우리 사이에는 정이 많다.' },
    { w: '눈치', p: 'nunchi', m: '眼色，察言观色', e: '눈치가 빠르다.' },
    { w: '한', p: 'han', m: '恨，郁结的情感', e: '한을 풀다.' },
    { w: '어머니', p: 'eomeoni', m: '母亲', e: '어머니가 보고 싶어요.' },
    { w: '화이팅', p: 'hwaiting', m: '加油', e: '오늘도 화이팅!' },
    { w: '정중앙', p: 'jeongjungang', m: '正中央', e: '정중앙에 서다.' },
  ],
};

const GRAMMAR = {
  en: [
    { q: 'Choose the correct form: "She ___ to Paris last summer."', opts: ['go', 'goes', 'went', 'gone'], a: 2, explain: 'last summer 表示过去，用一般过去时 went。' },
    { q: 'Fill: "If I ___ rich, I would travel the world."', opts: ['am', 'was', 'were', 'be'], a: 2, explain: '与现在事实相反的虚拟语气，be 动词用 were。' },
    { q: 'Which is correct?', opts: ['I have been to Japan.', 'I have went to Japan.', 'I has been to Japan.', 'I having been to Japan.'], a: 0, explain: '现在完成时 have + 过去分词 been。' },
    { q: 'Choose: "The book ___ by millions of people."', opts: ['reads', 'is read', 'is reading', 'read'], a: 1, explain: '被动语态 be + 过去分词，book 是被读。' },
    { q: 'Correct: "He is taller ___ his brother."', opts: ['then', 'than', 'that', 'as'], a: 1, explain: '比较级用 than。' },
  ],
  ja: [
    { q: '正しい助詞：「私___学生です。」', opts: ['は', 'が', 'を', 'に'], a: 0, explain: '主题提示用は。' },
    { q: '「食べる」の過去形は？', opts: ['食べます', '食べた', '食べて', '食べよう'], a: 1, explain: '动词た形表过去。' },
    { q: '敬語：「先生が___。」', opts: ['言います', 'おっしゃいます', '言う', '話す'], a: 1, explain: '尊敬语，言う 的尊敬形是おっしゃる。' },
  ],
  ko: [
    { q: '알맞은 조사: "저는 학생___입니다."', opts: ['이', '가', '은', '를'], a: 2, explain: '주어격 조사, 받침 있으면 은.' },
    { q: '"먹다"의 과거형은?', opts: ['먹어요', '먹었어요', '먹을 거예요', '먹고'], a: 1, explain: '과거형은 았/었어요.' },
    { q: '존댓말: "할아버지가___."', opts: ['먹어요', '잡수셨어요', '먹었다', '먹어'], a: 1, explain: '높임말, 먹다 → 잡수시다.' },
  ],
};

const SPEAK = {
  en: [
    { phrase: 'The best way to predict the future is to create it.', trans: '预测未来最好的方式是创造它。' },
    { phrase: 'Practice makes perfect, but nobody is perfect, so why practice?', trans: '熟能生巧，但没人完美，那为何练习？' },
    { phrase: 'Language is the road map of a culture.', trans: '语言是文化的地图。' },
  ],
  ja: [
    { phrase: '継続は力なり。', trans: '坚持就是力量。' },
    { phrase: '一期一会を大切にする。', trans: '珍惜一生一次的相遇。' },
  ],
  ko: [
    { phrase: '시작이 반이다.', trans: '开始是成功的一半。' },
    { phrase: '백지장도 맞들면 낫다.', trans: '三个臭皮匠赛过诸葛亮。' },
  ],
};

const LISTEN = {
  en: [
    { text: 'I would like to book a table for two at seven o\'clock.', q: 'What does the speaker want to do?', opts: ['Order food', 'Book a table', 'Cancel a reservation', 'Ask for directions'], a: 1 },
    { text: 'The meeting has been postponed until Friday afternoon.', q: 'When is the meeting?', opts: ['This morning', 'Friday afternoon', 'Tomorrow', 'Next Monday'], a: 1 },
    { text: 'Could you tell me how to get to the nearest subway station?', q: 'What is the speaker looking for?', opts: ['A bus stop', 'A taxi', 'A subway station', 'A parking lot'], a: 2 },
  ],
  ja: [
    { text: 'すみません、駅はどこですか。', q: '話者は何を探していますか？', opts: ['トイレ', '駅', 'レストラン', 'ホテル'], a: 1 },
    { text: '今日はとてもいい天気ですね。', q: '天気はどうですか？', opts: ['雨', '雪', 'いい天気', '曇り'], a: 2 },
  ],
  ko: [
    { text: '저기요, 화장실이 어디예요?', q: '화자는 무엇을 찾고 있나요?', opts: ['출구', '화장실', '역', '식당'], a: 1 },
    { text: '오늘 날씨가 정말 좋네요.', q: '날씨는 어떤가요?', opts: ['비', '눈', '좋음', '흐림'], a: 2 },
  ],
};

const ACHIEVEMENTS = [
  { id: 'first_login', icon: '🎯', name: '初次见面', desc: '首次登录', cond: s => s.logins >= 1 },
  { id: 'streak_3', icon: '🔥', name: '三日不辍', desc: '连续学习 3 天', cond: s => s.streak >= 3 },
  { id: 'streak_7', icon: '⚡', name: '一周坚持', desc: '连续学习 7 天', cond: s => s.streak >= 7 },
  { id: 'words_20', icon: '📚', name: '词汇新手', desc: '掌握 20 个单词', cond: s => s.wordsKnown >= 20 },
  { id: 'words_50', icon: '📖', name: '词汇达人', desc: '掌握 50 个单词', cond: s => s.wordsKnown >= 50 },
  { id: 'xp_100', icon: '⭐', name: '百分达人', desc: '累计 100 XP', cond: s => s.xp >= 100 },
  { id: 'xp_500', icon: '🌟', name: '闪耀之星', desc: '累计 500 XP', cond: s => s.xp >= 500 },
  { id: 'grammar_10', icon: '✏️', name: '语法练习生', desc: '完成 10 道语法题', cond: s => s.grammarDone >= 10 },
  { id: 'speak_5', icon: '🎤', name: '开口勇者', desc: '完成 5 次口语练习', cond: s => s.speakDone >= 5 },
];

const SEED_POSTS = [
  { author: '樱木', text: '今天终于把日语动词变形搞清楚了！推荐大家用「て形」串联记忆，效率翻倍。', tag: '日语', likes: 24, liked: false, time: '2 小时前' },
  { author: 'Luna', text: '练口语最大的障碍是害羞。打开录音，假装在跟朋友聊天，真的有效。', tag: '心得', likes: 51, liked: false, time: '5 小时前' },
  { author: 'Minji', text: '韩语的收音 받침 好难啊，有没有大佬分享练习方法？', tag: '韩语', likes: 12, liked: false, time: '昨天' },
  { author: 'Alex', text: 'Daily English tip: 读原版小说不要查每个词，先猜，再查，记忆更牢。', tag: '英语', likes: 38, liked: false, time: '昨天' },
];

const SEED_LEADERBOARD = [
  { name: 'Hana', xp: 2480 },
  { name: 'Jin', xp: 1920 },
  { name: 'Marco', xp: 1650 },
  { name: 'Yuki', xp: 1340 },
  { name: 'Sofia', xp: 1120 },
];

/* ---------- State ---------- */
const DEFAULT_USER = {
  email: '', name: '', lang: 'en',
  xp: 0, streak: 0, lastActive: null, logins: 0,
  wordsKnown: 0, grammarDone: 0, speakDone: 0, listenDone: 0,
  courseProgress: {},  // { 'en-A1': 0.3, ... }
  skills: { vocab: 0, grammar: 0, speak: 0, listen: 0 }, // 0-100
  activity: [], // { date: 'YYYY-MM-DD', minutes: n }
  unlockedAch: [],
  posts: SEED_POSTS,
};

let state = {
  currentLang: 'en',
  route: 'dashboard',
  user: null,
  session: null, // { module, items, index, score }
  demoMode: false,
};

/* ---------- Storage ---------- */
function saveUser() {
  if (state.user) localStorage.setItem('lingua_user', JSON.stringify(state.user));
}
function loadUser() {
  const raw = localStorage.getItem('lingua_user');
  return raw ? JSON.parse(raw) : null;
}
function clearUser() { localStorage.removeItem('lingua_user'); }

/* ---------- Toast ---------- */
function toast(msg, type = '') {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show ' + type;
  clearTimeout(t._tid);
  t._tid = setTimeout(() => t.classList.remove('show'), 2400);
}

/* ---------- Auth ---------- */
function switchAuthTab(tab) {
  document.querySelectorAll('.auth-tab').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
  document.getElementById('form-login').classList.toggle('active', tab === 'login');
  document.getElementById('form-register').classList.toggle('active', tab === 'register');
}

function handleLogin(e) {
  e.preventDefault();
  const fd = new FormData(e.target);
  const email = fd.get('email').trim();
  const pw = fd.get('password');
  const msg = document.getElementById('login-msg');
  msg.textContent = ''; msg.className = 'form-msg';

  // demo account
  if (email === 'demo@lingua.app' && pw === 'demo123') {
    state.user = { ...DEFAULT_USER, email, name: 'Demo 学员', lang: 'en' };
    saveUser(); enterApp();
    return;
  }

  const stored = localStorage.getItem('lingua_accounts');
  const accounts = stored ? JSON.parse(stored) : {};
  if (accounts[email] && accounts[email].password === pw) {
    const existing = loadUser();
    if (existing && existing.email === email) {
      state.user = existing;
    } else {
      state.user = { ...DEFAULT_USER, email, name: accounts[email].name, lang: accounts[email].lang };
      saveUser();
    }
    enterApp();
  } else {
    msg.textContent = '邮箱或密码错误';
    msg.className = 'form-msg error';
  }
}

function handleRegister(e) {
  e.preventDefault();
  const fd = new FormData(e.target);
  const name = fd.get('name').trim();
  const email = fd.get('email').trim();
  const pw = fd.get('password');
  const lang = fd.get('lang');
  const msg = document.getElementById('register-msg');
  msg.textContent = ''; msg.className = 'form-msg';

  const stored = localStorage.getItem('lingua_accounts');
  const accounts = stored ? JSON.parse(stored) : {};
  if (accounts[email]) {
    msg.textContent = '该邮箱已注册';
    msg.className = 'form-msg error';
    return;
  }
  accounts[email] = { name, password: pw, lang };
  localStorage.setItem('lingua_accounts', JSON.stringify(accounts));
  state.user = { ...DEFAULT_USER, email, name, lang };
  saveUser();
  toast('注册成功，欢迎加入 Lingua！', 'success');
  enterApp();
}

function enterApp() {
  state.user.logins = (state.user.logins || 0) + 1;
  state.currentLang = state.user.lang || 'en';
  updateStreak();
  saveUser();
  checkAchievements();
  document.getElementById('view-auth').classList.remove('active');
  document.getElementById('app-shell').classList.add('active');
  document.querySelector('.app-shell').classList.add('active');
  updateShellUI();
  navigate('dashboard');
}

function logout() {
  if (confirm('确定要退出登录吗？')) {
    state.user = null;
    clearUser();
    document.getElementById('app-shell').classList.remove('active');
    document.getElementById('view-auth').classList.add('active');
    document.getElementById('form-login').reset();
  }
}

function updateStreak() {
  const today = new Date().toISOString().slice(0, 10);
  const last = state.user.lastActive;
  if (last === today) return;
  if (last) {
    const d1 = new Date(last), d2 = new Date(today);
    const diff = Math.round((d2 - d1) / 86400000);
    if (diff === 1) state.user.streak = (state.user.streak || 0) + 1;
    else if (diff > 1) state.user.streak = 1;
  } else {
    state.user.streak = 1;
  }
  state.user.lastActive = today;
}

/* ---------- Shell UI ---------- */
function updateShellUI() {
  document.getElementById('sb-name').textContent = state.user.name;
  document.getElementById('sb-avatar').textContent = state.user.name.charAt(0).toUpperCase();
  document.getElementById('sb-streak').textContent = `🔥 ${state.user.streak || 0} 天`;
  document.getElementById('topbar-xp').textContent = state.user.xp;
  document.querySelectorAll('.ls-btn').forEach(b => b.classList.toggle('active', b.dataset.lang === state.currentLang));
  const labels = { dashboard: '主页', courses: '课程', learn: '练习', progress: '进度', community: '社区' };
  document.getElementById('page-title').textContent = labels[state.route];
}

/* ---------- Routing ---------- */
function navigate(route) {
  state.route = route;
  document.querySelectorAll('.nav-item').forEach(n => n.classList.toggle('active', n.dataset.route === route));
  document.querySelectorAll('.view-main').forEach(v => v.classList.remove('active'));
  const target = document.getElementById('view-' + route);
  if (target) target.classList.add('active');
  updateShellUI();
  if (route === 'learn') { document.getElementById('learn-home').classList.remove('hidden'); document.getElementById('learn-session').classList.add('hidden'); }
  renderRoute(route);
  window.scrollTo(0, 0);
}

function renderRoute(route) {
  const r = { dashboard: renderDashboard, courses: renderCourses, learn: renderLearn, progress: renderProgress, community: renderCommunity };
  if (r[route]) r[route]();
}

/* ---------- Dashboard ---------- */
function renderDashboard() {
  const u = state.user;
  document.getElementById('dash-greeting').textContent = `你好，${u.name}`;
  const hour = new Date().getHours();
  const greetDesc = hour < 12 ? '清晨的记忆最清晰，来背几个单词吧。' : hour < 18 ? '午后时光，练一段口语刚刚好。' : '晚上好，复盘今天学的内容。';
  document.getElementById('dash-desc').textContent = greetDesc;

  // daily goal: 20 xp per day
  const todayXP = (getActivityToday().xp);
  const goal = 20;
  const pct = Math.min(100, Math.round(todayXP / goal * 100));
  document.getElementById('daily-percent').textContent = pct + '%';
  const circ = 2 * Math.PI * 52;
  document.getElementById('daily-ring').style.strokeDashoffset = circ * (1 - pct / 100);

  document.getElementById('stat-learned').textContent = u.wordsKnown;
  document.getElementById('stat-streak').textContent = u.streak;
  document.getElementById('stat-minutes').textContent = getActivityToday().minutes;

  renderPath();
  renderDashAchievements();
}

function getActivityToday() {
  const today = new Date().toISOString().slice(0, 10);
  const rec = (state.user.activity || []).find(a => a.date === today);
  return rec || { xp: 0, minutes: 0 };
}

function renderPath() {
  const lang = state.currentLang;
  const u = state.user;
  const skills = u.skills || { vocab: 0, grammar: 0, speak: 0, listen: 0 };
  // sort skills ascending to recommend weakest-first learning
  const sorted = Object.entries(skills).sort((a, b) => a[1] - b[1]);
  const track = document.getElementById('path-track');
  const names = { vocab: '单词记忆', grammar: '语法练习', speak: '口语跟读', listen: '听力训练' };
  const descs = { vocab: '薄弱项优先，巩固核心词汇', grammar: '突破语法盲点', speak: '开口训练，提升流利度', listen: '强化听力理解' };
  track.innerHTML = sorted.map(([k], i) => {
    const done = skills[k] >= 70;
    const current = i === 0;
    return `<div class="path-node ${done ? 'done' : ''} ${current ? 'current' : ''}" data-module="${k}">
      <div class="pn-step">STEP ${i + 1}</div>
      <div class="pn-title">${names[k]}</div>
      <div class="pn-desc">${descs[k]}</div>
    </div>${i < sorted.length - 1 ? '<div class="path-arrow">→</div>' : ''}`;
  }).join('');
  track.querySelectorAll('.path-node').forEach(n => {
    n.onclick = () => { startSession(n.dataset.module); };
  });
  // recommendation reason
  const weakest = sorted[0][0];
  document.getElementById('path-reason').textContent = `检测到「${names[weakest]}」较薄弱，建议优先练习`;
}

function renderDashAchievements() {
  const u = state.user;
  const unlocked = new Set(u.unlockedAch || []);
  const row = document.getElementById('dash-achievements');
  row.innerHTML = ACHIEVEMENTS.slice(0, 6).map(a => `
    <div class="ach-badge ${unlocked.has(a.id) ? '' : 'locked'}">
      <div class="ab-icon">${a.icon}</div>
      <div class="ab-name">${a.name}</div>
    </div>`).join('');
}

/* ---------- Courses ---------- */
function renderCourses() {
  const lang = state.currentLang;
  const meta = LANG_META[lang];
  document.getElementById('course-lang-label').textContent = `${meta.name} · A1 → C1`;
  const list = COURSES[lang];
  const u = state.user;
  const wrap = document.getElementById('course-levels');
  wrap.innerHTML = list.map(c => {
    const key = `${lang}-${c.code}`;
    const pct = Math.round((u.courseProgress[key] || 0) * 100);
    return `<div class="course-lvl">
      <div class="lvl-code">${c.code}</div>
      <div class="lvl-info">
        <h4>${c.title}</h4>
        <p>${c.desc}</p>
      </div>
      <div class="lvl-progress">
        <div class="lp-bar"><div class="lp-fill" style="width:${pct}%"></div></div>
        <span>${pct}% 完成</span>
      </div>
      <button class="lvl-btn" data-lvl="${c.code}">${pct > 0 ? '继续' : '开始'}</button>
    </div>`;
  }).join('');
  wrap.querySelectorAll('.lvl-btn').forEach(b => {
    b.onclick = () => {
      const lvl = b.dataset.lvl;
      startCourseLesson(lang, lvl);
    };
  });
}

function startCourseLesson(lang, lvl) {
  // increment progress, grant xp
  const key = `${lang}-${lvl}`;
  const u = state.user;
  const cur = u.courseProgress[key] || 0;
  if (cur < 1) {
    u.courseProgress[key] = Math.min(1, cur + 0.15);
    addXP(10);
    logActivity(8);
    toast(`已完成 ${lang.toUpperCase()} ${lvl} 的一课时 (+10 XP)`, 'success');
  } else {
    toast('该级别已完成，去挑战下一级吧！');
  }
  saveUser();
  renderCourses();
  updateShellUI();
  checkAchievements();
}

/* ---------- Learn ---------- */
function renderLearn() {
  document.getElementById('learn-lang-label').textContent = `当前语言：${LANG_META[state.currentLang].name}`;
}

function startSession(module) {
  const lang = state.currentLang;
  let items = [];
  if (module === 'vocab') items = [...VOCAB[lang]].sort(() => Math.random() - 0.5);
  else if (module === 'grammar') items = [...GRAMMAR[lang]].sort(() => Math.random() - 0.5);
  else if (module === 'speak') items = [...SPEAK[lang]].sort(() => Math.random() - 0.5);
  else if (module === 'listen') items = [...LISTEN[lang]].sort(() => Math.random() - 0.5);
  if (!items.length) { toast('该语言暂无此模块内容'); return; }
  state.session = { module, items, index: 0, score: 0, total: items.length };
  document.getElementById('learn-home').classList.add('hidden');
  document.getElementById('learn-session').classList.remove('hidden');
  const titles = { vocab: '单词记忆', grammar: '语法练习', speak: '口语跟读', listen: '听力训练' };
  document.getElementById('session-title').textContent = titles[module];
  renderSessionItem();
}

function renderSessionItem() {
  const s = state.session;
  const item = s.items[s.index];
  const card = document.getElementById('session-card');
  const footer = document.getElementById('session-footer');
  document.getElementById('sp-fill').style.width = (s.index / s.total * 100) + '%';
  document.getElementById('sp-text').textContent = `${s.index + 1} / ${s.total}`;

  if (s.module === 'vocab') renderVocab(card, footer, item);
  else if (s.module === 'grammar') renderGrammar(card, footer, item);
  else if (s.module === 'speak') renderSpeak(card, footer, item);
  else if (s.module === 'listen') renderListen(card, footer, item);
}

function renderVocab(card, footer, item) {
  card.innerHTML = `
    <div class="flashcard" id="fc">
      <div class="fc-inner">
        <div class="fc-face front">
          <div class="fc-word">${item.w}</div>
          <div class="fc-phonetic">${item.p}</div>
          <div class="fc-hint">点击卡片查看释义</div>
        </div>
        <div class="fc-face back">
          <div class="fc-meaning">${item.m}</div>
          <div class="fc-example">${item.e}</div>
        </div>
      </div>
    </div>`;
  footer.innerHTML = `
    <button class="fc-btn unknown" data-ans="unknown">不认识</button>
    <button class="fc-btn fuzzy" data-ans="fuzzy">模糊</button>
    <button class="fc-btn known" data-ans="known">认识</button>`;
  document.getElementById('fc').onclick = () => document.getElementById('fc').classList.toggle('flipped');
  footer.querySelectorAll('.fc-btn').forEach(b => {
    b.onclick = () => finishVocabItem(b.dataset.ans);
  });
}

function finishVocabItem(ans) {
  const u = state.user;
  const gain = ans === 'known' ? 5 : ans === 'fuzzy' ? 3 : 1;
  addXP(gain);
  if (ans === 'known') u.wordsKnown = (u.wordsKnown || 0) + 1;
  u.skills.vocab = Math.min(100, (u.skills.vocab || 0) + (ans === 'known' ? 3 : ans === 'fuzzy' ? 1.5 : 0.5));
  logActivity(2);
  saveUser();
  nextSessionItem();
}

function renderGrammar(card, footer, item) {
  card.innerHTML = `
    <div class="q-prompt">语法练习</div>
    <div class="q-text">${item.q}</div>
    <div class="q-options">
      ${item.opts.map((o, i) => `<button class="q-opt" data-i="${i}">${String.fromCharCode(65 + i)}. ${o}</button>`).join('')}
    </div>
    <div class="q-explain" id="q-explain">${item.explain}</div>`;
  footer.innerHTML = `<button class="btn btn-ghost" id="skip-btn">跳过</button>`;
  let answered = false;
  card.querySelectorAll('.q-opt').forEach(btn => {
    btn.onclick = () => {
      if (answered) return;
      answered = true;
      const i = +btn.dataset.i;
      const correct = i === item.a;
      btn.classList.add(correct ? 'correct' : 'wrong');
      if (!correct) card.querySelector(`.q-opt[data-i="${item.a}"]`).classList.add('correct');
      card.querySelectorAll('.q-opt').forEach(b => b.classList.add('disabled'));
      document.getElementById('q-explain').classList.add('show');
      const u = state.user;
      u.grammarDone = (u.grammarDone || 0) + 1;
      u.skills.grammar = Math.min(100, (u.skills.grammar || 0) + (correct ? 4 : 1.5));
      if (correct) { state.session.score++; addXP(8); }
      else addXP(2);
      logActivity(2);
      saveUser();
      footer.innerHTML = `<button class="btn btn-primary" id="next-btn">下一题 →</button>`;
      document.getElementById('next-btn').onclick = nextSessionItem;
    };
  });
  document.getElementById('skip-btn').onclick = nextSessionItem;
}

function renderSpeak(card, footer, item) {
  card.innerHTML = `
    <div class="speak-wrap">
      <button class="btn btn-ghost" id="speak-play" style="margin-bottom:24px">🔊 听示范</button>
      <div class="speak-phrase">${item.phrase}</div>
      <div class="speak-trans">${item.trans}</div>
      <button class="record-btn" id="rec-btn">🎤</button>
      <div class="record-label" id="rec-label">点击录音跟读</div>
      <div class="score-circle hidden" id="score-circle">--</div>
    </div>`;
  footer.innerHTML = `<button class="btn btn-ghost" id="speak-skip">跳过</button>`;
  document.getElementById('speak-play').onclick = () => speak(item.phrase);
  let recording = false;
  const recBtn = document.getElementById('rec-btn');
  recBtn.onclick = () => {
    if (!recording) {
      recording = true;
      recBtn.classList.add('recording');
      document.getElementById('rec-label').textContent = '录音中... 再次点击结束';
      // simulate recording with SpeechRecognition if available
      startRecognition(item.phrase);
    } else {
      recording = false;
      recBtn.classList.remove('recording');
      document.getElementById('rec-label').textContent = '正在评分...';
      stopRecognition();
    }
  };
  document.getElementById('speak-skip').onclick = nextSessionItem;
}

let recognition = null;
function startRecognition(targetPhrase) {
  const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
  if (!SR) {
    // fallback: simulate after delay
    setTimeout(() => finishSpeak(Math.floor(Math.random() * 25) + 72), 1500);
    return;
  }
  try {
    recognition = new SR();
    recognition.lang = state.currentLang === 'en' ? 'en-US' : state.currentLang === 'ja' ? 'ja-JP' : 'ko-KR';
    recognition.continuous = false;
    recognition.interimResults = false;
    recognition.onresult = (e) => {
      const said = e.results[0][0].transcript.toLowerCase();
      const target = targetPhrase.toLowerCase().replace(/[^\p{L}\s]/gu, '').trim();
      const score = similarity(said, target);
      finishSpeak(score);
    };
    recognition.onerror = () => finishSpeak(Math.floor(Math.random() * 20) + 70);
    recognition.start();
  } catch {
    setTimeout(() => finishSpeak(Math.floor(Math.random() * 25) + 72), 1500);
  }
}
function stopRecognition() { if (recognition) { try { recognition.stop(); } catch {} } }

function similarity(a, b) {
  if (!a || !b) return 70;
  const wa = a.split(/\s+/), wb = b.split(/\s+/);
  let match = 0;
  wa.forEach(w => { if (wb.some(x => x === w || x.includes(w) || w.includes(x))) match++; });
  const pct = Math.round(match / Math.max(wa.length, wb.length) * 100);
  return Math.min(100, Math.max(60, pct + 10));
}

function finishSpeak(score) {
  document.getElementById('rec-label').textContent = '评分完成';
  const sc = document.getElementById('score-circle');
  sc.classList.remove('hidden');
  sc.textContent = score;
  sc.style.borderColor = score >= 80 ? 'var(--accent)' : score >= 60 ? 'var(--amber)' : 'var(--coral)';
  sc.style.color = score >= 80 ? 'var(--accent)' : score >= 60 ? 'var(--amber)' : 'var(--coral)';
  const u = state.user;
  u.speakDone = (u.speakDone || 0) + 1;
  u.skills.speak = Math.min(100, (u.skills.speak || 0) + score / 25);
  addXP(Math.round(score / 10));
  logActivity(3);
  saveUser();
  document.getElementById('session-footer').innerHTML = `<button class="btn btn-primary" id="next-btn">下一句 →</button>`;
  document.getElementById('next-btn').onclick = nextSessionItem;
}

function renderListen(card, footer, item) {
  card.innerHTML = `
    <div class="listen-wrap">
      <button class="listen-btn" id="listen-play">▶</button>
      <div class="q-prompt">听力理解</div>
      <div class="q-text">${item.q}</div>
      <div class="q-options">
        ${item.opts.map((o, i) => `<button class="q-opt" data-i="${i}">${String.fromCharCode(65 + i)}. ${o}</button>`).join('')}
      </div>
      <div class="listen-transcript" id="listen-transcript">原文：${item.text}</div>
    </div>`;
  footer.innerHTML = `
    <button class="btn btn-ghost" id="show-trans">显示原文</button>
    <button class="btn btn-ghost" id="skip-btn">跳过</button>`;
  document.getElementById('listen-play').onclick = () => speak(item.text);
  let answered = false;
  card.querySelectorAll('.q-opt').forEach(btn => {
    btn.onclick = () => {
      if (answered) return;
      answered = true;
      const i = +btn.dataset.i;
      const correct = i === item.a;
      btn.classList.add(correct ? 'correct' : 'wrong');
      if (!correct) card.querySelector(`.q-opt[data-i="${item.a}"]`).classList.add('correct');
      card.querySelectorAll('.q-opt').forEach(b => b.classList.add('disabled'));
      document.getElementById('listen-transcript').classList.add('show');
      const u = state.user;
      u.listenDone = (u.listenDone || 0) + 1;
      u.skills.listen = Math.min(100, (u.skills.listen || 0) + (correct ? 4 : 1.5));
      if (correct) { state.session.score++; addXP(8); }
      else addXP(2);
      logActivity(2);
      saveUser();
      footer.innerHTML = `<button class="btn btn-primary" id="next-btn">下一题 →</button>`;
      document.getElementById('next-btn').onclick = nextSessionItem;
    };
  });
  document.getElementById('show-trans').onclick = () => document.getElementById('listen-transcript').classList.add('show');
  document.getElementById('skip-btn').onclick = nextSessionItem;
}

function nextSessionItem() {
  const s = state.session;
  s.index++;
  if (s.index >= s.total) {
    finishSession();
    return;
  }
  renderSessionItem();
}

function finishSession() {
  const s = state.session;
  const card = document.getElementById('session-card');
  const footer = document.getElementById('session-footer');
  document.getElementById('sp-fill').style.width = '100%';
  document.getElementById('sp-text').textContent = `${s.total} / ${s.total}`;
  const pct = Math.round(s.score / s.total * 100);
  const titles = { vocab: '单词', grammar: '语法', speak: '口语', listen: '听力' };
  card.innerHTML = `
    <div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center">
      <div class="eyebrow">练习完成</div>
      <div style="font-size:48px;font-weight:800;margin:12px 0;color:var(--accent)">${pct}%</div>
      <div style="color:var(--ink-3);font-size:15px">${titles[s.module]}练习 · 正确 ${s.score}/${s.total}</div>
      <div style="margin-top:16px;font-size:13px;color:var(--amber)">继续保持，连续 ${state.user.streak} 天</div>
    </div>`;
  footer.innerHTML = `
    <button class="btn btn-ghost" id="again-btn">再来一组</button>
    <button class="btn btn-primary" id="done-btn">返回练习</button>`;
  document.getElementById('again-btn').onclick = () => startSession(s.module);
  document.getElementById('done-btn').onclick = () => {
    document.getElementById('learn-session').classList.add('hidden');
    document.getElementById('learn-home').classList.remove('hidden');
  };
  checkAchievements();
  updateShellUI();
}

/* ---------- TTS ---------- */
function speak(text) {
  if (!('speechSynthesis' in window)) { toast('当前浏览器不支持语音合成'); return; }
  speechSynthesis.cancel();
  const u = new SpeechSynthesisUtterance(text);
  u.lang = state.currentLang === 'en' ? 'en-US' : state.currentLang === 'ja' ? 'ja-JP' : 'ko-KR';
  u.rate = 0.9;
  speechSynthesis.speak(u);
}

/* ---------- XP & Activity ---------- */
function addXP(n) {
  state.user.xp = (state.user.xp || 0) + n;
  const el = document.getElementById('topbar-xp');
  if (el) el.textContent = state.user.xp;
}
function logActivity(minutes) {
  const u = state.user;
  const today = new Date().toISOString().slice(0, 10);
  u.activity = u.activity || [];
  let rec = u.activity.find(a => a.date === today);
  if (!rec) { rec = { date: today, minutes: 0, xp: 0 }; u.activity.push(rec); }
  rec.minutes += minutes;
  // keep last 14 days
  u.activity = u.activity.slice(-14);
}

/* ---------- Achievements ---------- */
function checkAchievements() {
  const u = state.user;
  u.unlockedAch = u.unlockedAch || [];
  ACHIEVEMENTS.forEach(a => {
    if (!u.unlockedAch.includes(a.id) && a.cond(u)) {
      u.unlockedAch.push(a.id);
      toast(`🏆 解锁成就：${a.name}`, 'achievement');
    }
  });
  saveUser();
}

/* ---------- Progress ---------- */
function renderProgress() {
  const u = state.user;
  document.getElementById('kpi-xp').textContent = u.xp;
  const weekXP = (u.activity || []).slice(-7).reduce((s, a) => s + (a.xp || 0), 0);
  document.getElementById('kpi-xp-delta').textContent = `+${weekXP} 本周`;
  document.getElementById('kpi-streak').textContent = u.streak;
  document.getElementById('kpi-words').textContent = u.wordsKnown;
  const lessons = Object.values(u.courseProgress || {}).filter(v => v > 0).length;
  document.getElementById('kpi-lessons').textContent = lessons;

  renderActivityChart();
  renderSkills();
  renderLangProgress();
}

function renderActivityChart() {
  const u = state.user;
  const days = [];
  for (let i = 13; i >= 0; i--) {
    const d = new Date();
    d.setDate(d.getDate() - i);
    const key = d.toISOString().slice(0, 10);
    const rec = (u.activity || []).find(a => a.date === key);
    days.push({ key, label: `${d.getMonth() + 1}/${d.getDate()}`, minutes: rec ? rec.minutes : 0 });
  }
  const max = Math.max(30, ...days.map(d => d.minutes));
  const svg = document.getElementById('activity-chart');
  const w = 600, h = 200, pad = 20;
  const stepX = (w - pad * 2) / (days.length - 1);
  const points = days.map((d, i) => {
    const x = pad + i * stepX;
    const y = h - pad - (d.minutes / max) * (h - pad * 2);
    return { x, y, ...d };
  });
  const pathD = points.map((p, i) => `${i === 0 ? 'M' : 'L'} ${p.x} ${p.y}`).join(' ');
  const areaD = pathD + ` L ${points[points.length - 1].x} ${h - pad} L ${points[0].x} ${h - pad} Z`;
  svg.innerHTML = `
    <defs>
      <linearGradient id="ag" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0%" stop-color="var(--accent)" stop-opacity="0.4"/>
        <stop offset="100%" stop-color="var(--accent)" stop-opacity="0"/>
      </linearGradient>
    </defs>
    <path d="${areaD}" fill="url(#ag)"/>
    <path d="${pathD}" fill="none" stroke="var(--accent)" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>
    ${points.map(p => `<circle cx="${p.x}" cy="${p.y}" r="3.5" fill="var(--accent)"/>`).join('')}
  `;
  document.getElementById('chart-axis').innerHTML = points.filter((_, i) => i % 2 === 0).map(p => `<span>${p.label}</span>`).join('');
}

function renderSkills() {
  const u = state.user;
  const skills = u.skills || { vocab: 0, grammar: 0, speak: 0, listen: 0 };
  const names = { vocab: '词汇', grammar: '语法', speak: '口语', listen: '听力' };
  document.getElementById('skill-grid').innerHTML = Object.entries(skills).map(([k, v]) => `
    <div class="skill-card">
      <h5>${names[k]}</h5>
      <div class="skill-bar"><div class="skill-fill" style="width:${Math.round(v)}%"></div></div>
      <small>掌握度 ${Math.round(v)}%</small>
    </div>`).join('');
}

function renderLangProgress() {
  const u = state.user;
  document.getElementById('lang-progress').innerHTML = Object.keys(LANG_META).map(lang => {
    const lvls = COURSES[lang];
    const avg = lvls.reduce((s, c) => s + (u.courseProgress[`${lang}-${c.code}`] || 0), 0) / lvls.length;
    const pct = Math.round(avg * 100);
    return `<div class="lp-row">
      <div class="lp-lang">${LANG_META[lang].name}</div>
      <div class="lp-bar"><div class="lp-fill" style="width:${pct}%"></div></div>
      <div class="lp-val">${pct}%</div>
    </div>`;
  }).join('');
}

/* ---------- Community ---------- */
function renderCommunity() {
  renderPosts();
  renderAchievements();
  renderLeaderboard();
}

function renderPosts() {
  const u = state.user;
  u.posts = u.posts || SEED_POSTS;
  const list = document.getElementById('post-list');
  list.innerHTML = u.posts.map((p, i) => `
    <div class="post">
      <div class="post-head">
        <div class="avatar">${p.author.charAt(0)}</div>
        <div>
          <div class="post-author">${p.author}</div>
          <div class="post-time">${p.time}</div>
        </div>
        <span class="post-tag">${p.tag}</span>
      </div>
      <div class="post-body">${p.text}</div>
      <div class="post-actions-row">
        <button class="${p.liked ? 'liked' : ''}" data-i="${i}" data-action="like">❤ ${p.likes}</button>
        <button>💬 回复</button>
        <button>↗ 分享</button>
      </div>
    </div>`).join('');
  list.querySelectorAll('[data-action="like"]').forEach(b => {
    b.onclick = () => {
      const i = +b.dataset.i;
      const p = u.posts[i];
      if (p.liked) { p.likes--; p.liked = false; } else { p.likes++; p.liked = true; }
      saveUser();
      renderPosts();
    };
  });
}

function submitPost() {
  const input = document.getElementById('post-input');
  const text = input.value.trim();
  if (!text) { toast('内容不能为空', 'error'); return; }
  const tag = document.getElementById('post-tag').value;
  state.user.posts.unshift({ author: state.user.name, text, tag, likes: 0, liked: false, time: '刚刚' });
  input.value = '';
  saveUser();
  renderPosts();
  toast('发布成功', 'success');
}

function renderAchievements() {
  const u = state.user;
  const unlocked = new Set(u.unlockedAch || []);
  document.getElementById('ach-grid').innerHTML = ACHIEVEMENTS.map(a => `
    <div class="ach-badge ${unlocked.has(a.id) ? '' : 'locked'}" title="${a.desc}">
      <div class="ab-icon">${a.icon}</div>
      <div class="ab-name">${a.name}</div>
    </div>`).join('');
}

function renderLeaderboard() {
  const u = state.user;
  const board = [...SEED_LEADERBOARD, { name: u.name + ' (你)', xp: u.xp }]
    .sort((a, b) => b.xp - a.xp)
    .slice(0, 6);
  document.getElementById('leaderboard').innerHTML = board.map((p, i) => `
    <li class="${i < 3 ? 'top' : ''}">
      <span class="lb-name">${p.name}</span>
      <span class="lb-xp">${p.xp} XP</span>
    </li>`).join('');
}

/* ---------- Init ---------- */
function init() {
  // auth tabs
  document.querySelectorAll('.auth-tab').forEach(b => b.onclick = () => switchAuthTab(b.dataset.tab));
  document.getElementById('form-login').addEventListener('submit', handleLogin);
  document.getElementById('form-register').addEventListener('submit', handleRegister);
  document.getElementById('btn-logout').onclick = logout;

  // nav
  document.querySelectorAll('[data-route]').forEach(el => {
    el.onclick = () => navigate(el.dataset.route);
  });

  // lang switch
  document.querySelectorAll('.ls-btn').forEach(b => b.onclick = () => {
    state.currentLang = b.dataset.lang;
    if (state.user) state.user.lang = b.dataset.lang;
    saveUser();
    updateShellUI();
    renderRoute(state.route);
  });

  // module cards
  document.querySelectorAll('.module-card').forEach(c => {
    c.onclick = () => startSession(c.dataset.module);
  });

  // community
  document.getElementById('post-submit').onclick = submitPost;

  // learn back
  document.getElementById('learn-back').onclick = () => {
    document.getElementById('learn-session').classList.add('hidden');
    document.getElementById('learn-home').classList.remove('hidden');
  };

  // auto-login if user exists
  const u = loadUser();
  if (u) {
    state.user = u;
    document.getElementById('view-auth').classList.remove('active');
    document.getElementById('app-shell').classList.add('active');
    updateShellUI();
    navigate('dashboard');
  }
}

document.addEventListener('DOMContentLoaded', init);
