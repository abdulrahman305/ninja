#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include <assert.h>

using namespace std;

struct Node;
struct FileStat {
  FileStat(const string& path) : path_(path), mtime_(0), node_(NULL) {}
  void Touch(int mtime);
  string path_;
  int mtime_;
  Node* node_;
};

struct Edge;
struct Node {
  Node(FileStat* file) : file_(file), dirty_(false), in_edge_(NULL) {}

  bool dirty() const { return dirty_; }
  void MarkDirty();

  FileStat* file_;
  bool dirty_;
  Edge* in_edge_;
  vector<Edge*> out_edges_;
};

struct EvalString {
  struct Env {
    virtual string Evaluate(const string& var) = 0;
  };
  bool Parse(const string& input);
  string Evaluate(Env* env);

  const string& unparsed() const { return unparsed_; }

  string unparsed_;
  enum TokenType { RAW, SPECIAL };
  typedef vector<pair<string, TokenType> > TokenList;
  TokenList parsed_;
};

bool EvalString::Parse(const string& input) {
  unparsed_ = input;

  string::size_type start, end;
  start = 0;
  do {
    end = input.find_first_of("@$", start);
    if (end == string::npos) {
      end = input.size();
      break;
    }
    if (end > start)
      parsed_.push_back(make_pair(input.substr(start, end - start), RAW));
    start = end;
    for (end = start + 1; end < input.size(); ++end) {
      if (!('a' <= input[end] && input[end] <= 'z'))
        break;
    }
    if (end == start + 1) {
      // XXX report bad parse here
      return false;
    }
    parsed_.push_back(make_pair(input.substr(start, end - start), SPECIAL));
    start = end;
  } while (end < input.size());
  if (end > start)
    parsed_.push_back(make_pair(input.substr(start, end - start), RAW));

  return true;
}

string EvalString::Evaluate(Env* env) {
  string result;
  for (TokenList::iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
    if (i->second == RAW)
      result.append(i->first);
    else
      result.append(env->Evaluate(i->first));
  }
  return result;
}

struct Rule {
  Rule(const string& name, const string& command) :
    name_(name) {
    assert(command_.Parse(command));  // XXX
  }
  string name_;
  EvalString command_;
};

struct Edge {
  Edge() : rule_(NULL) {}

  void MarkDirty(Node* node);
  string EvaluateCommand();  // XXX move to env, take env ptr

  Rule* rule_;
  enum InOut { IN, OUT };
  vector<Node*> inputs_;
  vector<Node*> outputs_;
};

void FileStat::Touch(int mtime) {
  if (node_)
    node_->MarkDirty();
}

void Node::MarkDirty() {
  if (dirty_)
    return;  // We already know.
  dirty_ = true;
  for (vector<Edge*>::iterator i = out_edges_.begin(); i != out_edges_.end(); ++i)
    (*i)->MarkDirty(this);
}

void Edge::MarkDirty(Node* node) {
  vector<Node*>::iterator i = find(inputs_.begin(), inputs_.end(), node);
  if (i == inputs_.end())
    return;
  for (i = outputs_.begin(); i != outputs_.end(); ++i)
    (*i)->MarkDirty();
}

struct EdgeEnv : public EvalString::Env {
  EdgeEnv(Edge* edge) : edge_(edge) {}
  virtual string Evaluate(const string& var) {
    string result;
    if (var == "@in") {
      for (vector<Node*>::iterator i = edge_->inputs_.begin();
           i != edge_->inputs_.end(); ++i) {
        if (!result.empty())
          result.push_back(' ');
        result.append((*i)->file_->path_);
      }
    } else if (var == "$out") {
      result = edge_->outputs_[0]->file_->path_;
    }
    return result;
  }
  Edge* edge_;
};

string Edge::EvaluateCommand() {
  EdgeEnv env(this);
  return rule_->command_.Evaluate(&env);
}

struct StatCache {
  typedef map<string, FileStat*> Paths;
  Paths paths_;
  FileStat* GetFile(const string& path);
};

FileStat* StatCache::GetFile(const string& path) {
  Paths::iterator i = paths_.find(path);
  if (i != paths_.end())
    return i->second;
  FileStat* file = new FileStat(path);
  paths_[path] = file;
  return file;
}

struct State {
  StatCache stat_cache_;
  map<string, Rule*> rules_;
  vector<Edge*> edges_;

  StatCache* stat_cache() { return &stat_cache_; }

  Rule* AddRule(const string& name, const string& command);
  Edge* AddEdge(Rule* rule);
  Edge* AddEdge(const string& rule_name);
  Node* GetNode(const string& path);
  void AddInOut(Edge* edge, Edge::InOut inout, const string& path);
};

Rule* State::AddRule(const string& name, const string& command) {
  Rule* rule = new Rule(name, command);
  rules_[name] = rule;
  return rule;
}

Edge* State::AddEdge(const string& rule_name) {
  return AddEdge(rules_[rule_name]);
}

Edge* State::AddEdge(Rule* rule) {
  Edge* edge = new Edge();
  edge->rule_ = rule;
  edges_.push_back(edge);
  return edge;
}

Node* State::GetNode(const string& path) {
  FileStat* file = stat_cache_.GetFile(path);
  if (!file->node_)
    file->node_ = new Node(file);
  return file->node_;
}

void State::AddInOut(Edge* edge, Edge::InOut inout, const string& path) {
  Node* node = GetNode(path);
  if (inout == Edge::IN) {
    edge->inputs_.push_back(node);
    node->out_edges_.push_back(edge);
  } else {
    edge->outputs_.push_back(node);
    assert(node->in_edge_ == NULL);
    node->in_edge_ = edge;
  }
}

struct Plan {
  Plan(State* state) : state_(state) {}

  void AddTarget(const string& path);
  bool AddTarget(Node* node);

  Edge* FindWork();

  State* state_;
  set<Node*> want_;
  queue<Edge*> ready_;
};

void Plan::AddTarget(const string& path) {
  AddTarget(state_->GetNode(path));
}
bool Plan::AddTarget(Node* node) {
  if (!node->dirty())
    return false;
  Edge* edge = node->in_edge_;
  if (!edge) {
    // TODO: if file doesn't exist we should die here.
    return false;
  }

  want_.insert(node);

  bool awaiting_inputs = false;
  for (vector<Node*>::iterator i = edge->inputs_.begin(); i != edge->inputs_.end(); ++i) {
    if (AddTarget(*i))
      awaiting_inputs = true;
  }

  if (!awaiting_inputs)
    ready_.push(edge);

  return true;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;
  Edge* edge = ready_.front();
  ready_.pop();
  return edge;
}

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct ManifestParser {
  ManifestParser(State* state) : state_(state), line_(0), col_(0) {}
  bool Load(const string& filename, string* err);
  bool Parse(const string& input, string* err);

  bool Error(const string& message, string* err);

  bool ParseRule(string* err);
  bool ParseEdge(string* err);

  bool SkipWhitespace(bool newline=false);
  bool Newline(string* err);
  bool NextToken();
  bool ReadToNewline(string* text, string* err);

  State* state_;
  const char* cur_;
  const char* end_;
  int line_, col_;
  string token_;
};

bool ManifestParser::Load(const string& filename, string* err) {
  FILE* f = fopen(filename.c_str(), "r");
  if (!f) {
    err->assign(strerror(errno));
    return false;
  }

  string text;
  char buf[64 << 10];
  size_t len;
  while ((len = fread(buf, 1, sizeof(buf), f)) > 0) {
    text.append(buf, len);
  }
  if (ferror(f)) {
    err->assign(strerror(errno));
    fclose(f);
    return false;
  }
  fclose(f);

  return Parse(text, err);
}

bool ManifestParser::Parse(const string& input, string* err) {
  cur_ = input.data(); end_ = cur_ + input.size();
  line_ = col_ = 0;

  while (NextToken()) {
    if (token_ == "rule") {
      if (!ParseRule(err))
        return false;
    } else if (token_ == "build") {
      if (!ParseEdge(err))
        return false;
    } else {
      return Error("unknown token: " + token_, err);
    }
    SkipWhitespace(true);
  }

  if (cur_ < end_)
    return Error("expected eof", err);

  return true;
}

bool ManifestParser::Error(const string& message, string* err) {
  char buf[1024];
  sprintf(buf, "line %d, col %d: %s", line_ + 1, col_ + 1, message.c_str());
  err->assign(buf);
  return false;
}

bool ManifestParser::ParseRule(string* err) {
  SkipWhitespace();
  if (!NextToken())
    return Error("expected rule name", err);
  if (!Newline(err))
    return false;
  string name = token_;

  if (!NextToken() || token_ != "command")
    return Error("expected command", err);
  string command;
  SkipWhitespace();
  if (!ReadToNewline(&command, err))
    return false;

  state_->AddRule(name, command);

  return true;
}

bool ManifestParser::ParseEdge(string* err) {
  vector<string> ins, outs;
  string rule;
  SkipWhitespace();
  for (;;) {
    if (!NextToken())
      return Error("expected output file list", err);
    if (token_ == ":")
      break;
    ins.push_back(token_);
  }
  if (!NextToken())
    return Error("expected build command name", err);
  rule = token_;
  for (;;) {
    if (!NextToken())
      break;
    outs.push_back(token_);
  }
  if (!Newline(err))
    return false;

  Edge* edge = state_->AddEdge(rule);
  if (!edge)
    return Error("unknown build rule name name", err);
  for (vector<string>::iterator i = ins.begin(); i != ins.end(); ++i)
    state_->AddInOut(edge, Edge::IN, *i);
  for (vector<string>::iterator i = outs.begin(); i != outs.end(); ++i)
    state_->AddInOut(edge, Edge::OUT, *i);

  return true;
}

bool ManifestParser::SkipWhitespace(bool newline) {
  bool skipped = false;
  while (cur_ < end_) {
    if (*cur_ == ' ') {
      ++col_;
    } else if (newline && *cur_ == '\n') {
      col_ = 0; ++line_;
    } else {
      break;
    }
    skipped = true;
    ++cur_;
  }
  return skipped;
}

bool ManifestParser::Newline(string* err) {
  if (cur_ < end_ && *cur_ == '\n') {
    ++cur_; ++line_; col_ = 0;
    return true;
  } else {
    return Error("expected newline", err);
  }
}

static bool IsIdentChar(char c) {
  return
    ('a' <= c && c <= 'z') ||
    ('0' <= c && c <= '9');
}

bool ManifestParser::NextToken() {
  SkipWhitespace();
  token_.clear();
  if (cur_ >= end_)
    return false;

  if (IsIdentChar(*cur_)) {
    while (cur_ < end_ && IsIdentChar(*cur_)) {
      token_.push_back(*cur_);
      ++col_; ++cur_;
    }
  } else if (*cur_ == ':') {
    token_ = ":";
    ++col_; ++cur_;
  }

  return !token_.empty();
}

bool ManifestParser::ReadToNewline(string* text, string* err) {
  while (cur_ < end_ && *cur_ != '\n') {
    text->push_back(*cur_);
    ++cur_; ++col_;
  }
  return Newline(err);
}
