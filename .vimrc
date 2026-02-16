" Project-local Vim config for huap
" Requires: set exrc secure in ~/.vimrc

let g:huap_make = 'make compile'
let g:huap_debug_executable = './huap'
let g:huap_run_workdir = 'content'
let g:huap_arguments = '../docs'
let g:huap_envs = {}

set makeprg=make\ compile
set errorformat=%f:%l:%c:\ %m,%f:%l:\ %m
packadd termdebug

let g:termdebug_config = {}
let g:termdebug_config['variables_window'] = v:true

syntax on
filetype plugin indent on
set noswapfile

nnoremap <leader>x :call LocalRun()<CR>
nnoremap <leader>c :call LocalMake()<CR>
nnoremap <leader>m :call LocalDebugMain()<CR>
nnoremap <leader>l :call LocalDebugLine()<CR>
nnoremap <leader>d :call LocalDev()<CR>

augroup HuapFiletypes
  autocmd!
  autocmd FileType markdown setlocal wrap linebreak spell spelllang=en_us textwidth=80 formatoptions+=t
  autocmd FileType markdown setlocal comments=fb:*,fb:-,fb:+,n:> listchars=tab:>-,trail:.
  autocmd FileType html setlocal shiftwidth=2 softtabstop=2 tabstop=2 expandtab textwidth=0
  autocmd FileType html setlocal matchpairs+=<:>
augroup END

function! s:ApplyDebugEnvs() abort
  for [k, v] in items(g:huap_envs)
    call TermDebugSendCommand(printf('set env %s %s', k, v))
  endfor
endfunction

function! LocalRun() abort
  let envs = join(map(items(g:huap_envs), { _, kv -> kv[0] . '=' . shellescape(kv[1]) }), ' ')
  if !empty(envs)
    let envs .= ' '
  endif

  let cmd = printf('cd %s && %s../huap %s', shellescape(g:huap_run_workdir), envs, g:huap_arguments)
  execute 'term ++curwin sh -lc ' . shellescape(cmd)
endfunction

function! LocalDebugMain() abort
  execute printf('Termdebug %s %s', g:huap_debug_executable, g:huap_arguments)
  call TermDebugSendCommand(printf('cd %s', g:huap_run_workdir))
  call s:ApplyDebugEnvs()
  call TermDebugSendCommand('break main')
  call TermDebugSendCommand('run')
endfunction

function! LocalDebugLine() abort
  let cmd = printf('break %s:%d', expand('%:p'), line('.'))
  execute printf('Termdebug %s %s', g:huap_debug_executable, g:huap_arguments)
  call TermDebugSendCommand(printf('cd %s', g:huap_run_workdir))
  call s:ApplyDebugEnvs()
  call TermDebugSendCommand(cmd)
  call TermDebugSendCommand('run')
endfunction

function! LocalMake() abort
  for [k, v] in items(g:huap_envs)
    execute printf('let $%s = %s', k, string(v))
  endfor

  silent make

  let qfl = getqflist()
  let filtered = filter(copy(qfl), { _, entry -> entry.valid == 1 })
  call setqflist(filtered, 'r')

  redraw!
  execute len(filtered) > 0 ? 'copen' : 'cclose'
endfunction

function! LocalDev() abort
  belowright split
  resize 12
  execute 'term ++curwin sh -lc ' . shellescape('make dev')
endfunction
