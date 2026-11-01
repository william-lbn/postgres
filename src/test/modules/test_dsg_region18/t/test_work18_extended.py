#!/usr/bin/env python3
# Work2本批新增extended门禁与512固定边界；只实际PG进程，不授normal许可。
import json,struct,unittest
import test_work18 as work

def cstr(s):return s.encode()+b'\0'
def msg(kind,body=b''):return kind.encode()+struct.pack('!I',len(body)+4)+body
def parse(name,sql):return cstr(name)+cstr(sql)+struct.pack('!h',0)
def bind(portal,statement):return cstr(portal)+cstr(statement)+struct.pack('!hhh',0,0,0)
def describe(kind,name):return kind.encode()+cstr(name)
def execute(portal):return cstr(portal)+struct.pack('!I',0)
def integer_rows(rows):
 values=[]
 for row in rows:
  if struct.unpack('!h',row[:2])[0]!=1:raise AssertionError('expected one actual PG column')
  size=struct.unpack('!i',row[2:6])[0];values.append(int(row[6:6+size]))
 return values

class Extended(unittest.TestCase):
 cluster=work.Work.cluster
 snap=work.Work.snap
 wait=work.Work.wait
 wire=work.Work.wire
 fact=work.Work.fact
 def cycle(self,w,items):
  w.socket.sendall(b''.join(msg(k,b)for k,b in items)+msg('S'));return w.ready()
 def admission(self,p,opened):p.sql('SELECT test_dsg_work18_admission('+('true'if opened else'false')+')')
 def test_PG18_S_WORK2_005_extended_original_phases_close_sync_reopen(self):
  observations=[]
  for phase in ['P','B','D','E']:
   with self.subTest(phase=phase):
    p=self.cluster();p.sql('CREATE SEQUENCE work18_sequence;GRANT USAGE,SELECT ON SEQUENCE work18_sequence TO work18_user');w=self.wire(p);self.admission(p,True)
    errors,rows,_=self.cycle(w,[('P',parse('s_probe',"SELECT nextval('work18_sequence')")),('B',bind('','s_probe')),('D',describe('P','')),('E',execute(''))]);self.assertFalse(errors);self.assertEqual(integer_rows(rows),[1])
    if phase=='E':
     self.assertFalse(w.query('BEGIN')[0]);self.assertFalse(self.cycle(w,[('B',bind('p_pending','s_probe'))])[0]);self.wait(p,lambda s:s['retained_transactions']==1)
    body={'P':parse('s_late',"SELECT nextval('work18_sequence')"),'B':bind('','s_probe'),'D':describe('S','s_probe'),'E':execute('p_pending')}[phase]
    self.admission(p,False)
    # 只给类型/长度与首正文byte，真实观察普通工作在正文完整到达前已经进入有界拒帧。
    w.socket.sendall(phase.encode()+struct.pack('!I',len(body)+4)+body[:1]);bounded=self.wait(p,lambda s:s['bounded_inputs']==1 and s['frontend_reads']==0)
    w.socket.sendall(body[1:]+msg('S'));errors,_,ready=w.ready();self.assertEqual(len(errors),1);self.assertIn('55000',errors[0]);self.assertIn('admission is closed',errors[0])
    self.assertEqual(p.sql('SELECT last_value FROM work18_sequence').stdout.strip(),'1','closed Execute must not consume nontransactional sequence state')
    if phase=='E':self.assertEqual(ready,b'E');self.assertFalse(w.query('ROLLBACK')[0])
    else:self.assertEqual(ready,b'I')
    idle=self.wait(p,lambda s:s['bounded_inputs']==0 and s['idle_clients']==1 and s['frontend_executes']==0)
    self.admission(p,True)
    items=[]
    if phase=='P':items.append(('P',parse('s_late',"SELECT nextval('work18_sequence')")))
    if phase=='D':self.assertFalse(self.cycle(w,[('D',describe('S','s_probe'))])[0])
    items += [('B',bind('','s_late'if phase=='P'else's_probe')),('E',execute(''))]
    errors,rows,_=self.cycle(w,items);self.assertFalse(errors);self.assertEqual(integer_rows(rows),[2])
    errors,rows,_=self.cycle(w,[('P',parse('s_identity','SELECT pg_backend_pid()')),('B',bind('','s_identity')),('E',execute(''))]);self.assertFalse(errors);self.assertEqual(integer_rows(rows),[w.pid])
    observations.append({'phase':phase,'original_backend_pid':w.pid,'closed_body_bytes':len(body),'actual_bounded_before_full_body':bounded,'actual_idle_after_sync':idle,'sequence_before_closed':1,'sequence_after_reopen':2})
  self.fact('work2-extended-four-phase-actual',observations)

 def test_PG18_S_WORK2_006_fixed512_transaction_finish_query_and_extended(self):
  facts=[]
  for length in [512,513]:
   with self.subTest(body_bytes=length):
    p=self.cluster();w=self.wire(p)
    self.assertFalse(w.query('BEGIN')[0]);self.admission(p,False)
    body=b'ROLLBACK'+b' '*(length-9)+b'\0';self.assertEqual(len(body),length)
    w.socket.sendall(msg('Q',body));errors,_,ready=w.ready()
    if length==512:self.assertFalse(errors);self.assertEqual(ready,b'I')
    else:
     self.assertEqual(len(errors),1);self.assertIn('55000',errors[0]);self.assertEqual(ready,b'E');self.assertEqual(self.snap(p)['retained_transactions'],1)
     exact=b'ROLLBACK'+b' '*(512-9)+b'\0';w.socket.sendall(msg('Q',exact));self.assertFalse(w.ready()[0])
    self.wait(p,lambda s:s['retained_transactions']==0)
    self.admission(p,True);self.assertFalse(w.query('BEGIN')[0]);self.admission(p,False)
    # P body = unnamed1 + queryCString + int16paramCount2；512精确边界涵盖真实P/B/D/E结束链。
    sql='ROLLBACK'+' '*(length-12);parsed=parse('',sql);self.assertEqual(len(parsed),length)
    errors,_,ready=self.cycle(w,[('P',parsed),('D',describe('S','')),('B',bind('','')),('D',describe('P','')),('E',execute(''))])
    if length==512:self.assertFalse(errors);self.assertEqual(ready,b'I')
    else:
     self.assertEqual(len(errors),1);self.assertIn('55000',errors[0]);self.assertEqual(ready,b'E');self.assertEqual(self.snap(p)['retained_transactions'],1)
     self.assertFalse(w.query('ROLLBACK')[0])
    closed=self.wait(p,lambda s:s['retained_transactions']==0 and s['bounded_inputs']==0 and s['foreground_idle'])
    self.admission(p,True);self.assertFalse(w.query('SELECT 18')[0]);facts.append({'body_bytes':length,'query_and_parse_accepted':length==512,'actual_closed_after_end':closed,'same_original_backend_pid':w.pid})
  self.fact('work2-fixed512-and-513-actual',facts)

if __name__=='__main__':unittest.main(verbosity=2)
